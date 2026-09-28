/*************************************************************************/ /*!
@Title          Kernel side command queue functions
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

#include "services_headers.h"
#include "pvr_bridge_km.h"

#include "lists.h"
#include "ttrace.h"

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0))
#include <linux/sw_sync.h>
#else
#include <sw_sync.h>
#endif

static struct sync_fence *AllocQueueFence(struct sw_sync_timeline *psTimeline, IMG_UINT32 ui32FenceValue, const char *szName)
{
	struct sync_fence *psFence = IMG_NULL;
	struct sync_pt *psPt;

	psPt = sw_sync_pt_create(psTimeline, ui32FenceValue);
	if(psPt)
	{
		psFence = sync_fence_create(szName, psPt);
		if(!psFence)
		{
			sync_pt_free(psPt);
		}
	}

	return psFence;
}
#endif /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC) */

/*
 * The number of commands of each type which can be in flight at once.
 */

#define DC_MAX_SUPPORTED_QUEUES			1
#if defined(SUPPORT_DC_CMDCOMPLETE_WHEN_NO_LONGER_DISPLAYED)
#define DC_NUM_COMMANDS_PER_QUEUE              2
#else
#define DC_NUM_COMMANDS_PER_QUEUE              1
#endif

#define DC_NUM_COMMANDS_PER_TYPE (DC_NUM_COMMANDS_PER_QUEUE * DC_MAX_SUPPORTED_QUEUES)

static IMG_UINT32 ui32NoOfSwapchainCreated = 0;


#define DC_NUM_CMDS_COMPLETE_HISTORY  40 /*must be multiple of 4*/
/*
 * List of private command processing function pointer tables and command
 * complete tables for a device in the system.
 * Each table is allocated when the device registers its private command
 * processing functions.
 */
typedef struct _DEVICE_COMMAND_DATA_
{
	PFN_CMD_PROC			pfnCmdProc;
	PCOMMAND_COMPLETE_DATA	apsCmdCompleteData[DC_NUM_COMMANDS_PER_TYPE];
	IMG_UINT32				ui32CCBOffset;
	IMG_UINT32				ui32MaxDstSyncCount;	/*!< Maximum number of dest syncs */
	IMG_UINT32				ui32MaxSrcSyncCount;	/*!< Maximum number of source syncs */
	unsigned long           aulCompTimeHis[DC_NUM_CMDS_COMPLETE_HISTORY];
	IMG_UINT32              ui32THWriteOffset;
} DEVICE_COMMAND_DATA;

/*
 * Cycle 4/5/7: command-processor lifetime state, per device.
 *
 *  CMD_PROC_STATE_DETACHED: no table published (initial state; also the
 *      state after detach/free). Admission rejected, no dispatch.
 *  CMD_PROC_STATE_RUNNING:  table published, admission open.
 *  CMD_PROC_STATE_CLOSING:  table still published, admission rejected
 *      (freeze). Already-queued commands keep dispatching and completing
 *      naturally; it is a reversible state (Unfreeze).
 *
 * A single global pointer NULL is therefore no longer overloaded: callers
 * that must distinguish "closed but still live" from "detached" read this
 * state word instead of inferring from apsDeviceCommandData[].
 */
typedef enum
{
	CMD_PROC_STATE_DETACHED = 0,
	CMD_PROC_STATE_RUNNING  = 1,
	CMD_PROC_STATE_CLOSING  = 2
} CMD_PROC_STATE;

static atomic_t sCmdProcState[SYS_DEVICE_COUNT];

/*
 * Cycle 4/5: reader tracking for command-processor lifetime.
 *
 * Queue-processing readers (PVRSRVProcessCommand) run under the shared
 * sQProcessResource lifecycle lock, so removal holding that lock already
 * excludes them. Completions (PVRSRVCommandCompleteStatusKM), inserts
 * (PVRSRVInsertCommandKM), display-swap admission pins (deviceclass.c) and
 * the /proc dump can run outside that lock, so they pin the table with this
 * atomic reader count across each apsDeviceCommandData load+use window:
 *
 *   reader: atomic_inc, smp_mb, load pointer (NULL-safe use), smp_mb, atomic_dec
 *   freer:  store NULL, smp_mb, wait until count is 0, then free
 *
 * The barriers order publication only; they are NOT a substitute for
 * quiescence. Real quiescence comes from an admission freeze plus the pin
 * drain: after the freeze no new admitter can pin, old pins drain, and only
 * then is the table detached/freed. Post-detach readers observe NULL and
 * never dereference.
 *
 * No sleeping lock is taken by readers, so this cannot deadlock with queue
 * drain, ProcessFlip or completion callbacks. Cookies (COMMAND_COMPLETE_DATA
 * entries) are additionally protected by the bInUse protocol: detach refuses
 * while any entry is in use, and completions are exactly-once by caller
 * contract.
 */
static atomic_t sCmdProcRef[SYS_DEVICE_COUNT];

/*
 * Cycle 7: submitted-command ownership, per device.
 *
 * A command is incremented here in PVRSRVSubmitCommandKM before the queue
 * write offset is published, and decremented in PVRSRVProcessQueues after
 * PVRSRVProcessCommand has terminally popped it (UPDATE_QUEUE_ROFF). The
 * count therefore covers exactly the "submitted but not yet popped" window,
 * including dependency-blocked commands that no dispatch can retire yet.
 *
 * Detach requires this count to be zero (checked under sQProcessResource),
 * so a command-processor table can never be unlinked while a command that
 * still needs it sits in a Services queue. Every terminal path pops through
 * the single decrement site; insertion failures never increment (increment
 * happens at submit); queue-destroy RETRY retains the count because the
 * queue is retained.
 */
static atomic_t sCmdProcQueued[SYS_DEVICE_COUNT];

/*
 * Detached-but-not-yet-freed tables, owned by the detach/free split
 * protocol (PVRSRVDetachCmdProcKM hands off to PVRSRVFreeDetachedCmdProcKM).
 * Only the detacher writes its slot; only the freer consumes it.
 */
static DEVICE_COMMAND_DATA *sDetachedCmdProc[SYS_DEVICE_COUNT];

/*
 * Cycle 8: the exact table pointer captured when the device was frozen.
 * Detach parks/frees only this table, never whatever happens to be at
 * apsDeviceCommandData[] later. This closes the device-ID reuse window by
 * construction: even if the Services device ID were recycled and some path
 * published another command-processor table at the same index while this
 * teardown is staged, the old detach refuses (it does not unmap, free or
 * overwrite the new table). The DC lifecycle independently prevents such a
 * re-registration (STAGED refuses Init), so this is defense in depth.
 */
static DEVICE_COMMAND_DATA *sFrozenCmdProc[SYS_DEVICE_COUNT];

/* Bounded quiescence wait for non-queue-lock readers (1ms * count). */
#define CMDPROC_REF_DRAIN_MS	500u

/*
 * Cycle 6/7 table pin (completions, inserts, dump): takes a reference only
 * when a table is published, so a NULL return never owns a reference and
 * callers must release exactly when (and only when) a non-NULL table was
 * returned.
 *
 *   p = load(aps);            // optimistic, no dereference: always safe
 *   if (!p) return NULL;      // no ref taken, nothing to release
 *   inc; mb;
 *   if (load(aps) != p) {     // lost a race with detach
 *       mb; dec; return NULL; // fully unwound here, not by the caller
 *   }
 *   return p;                 // pinned + validated: caller must release
 */
static DEVICE_COMMAND_DATA *CmdProcPinGet(SYS_DATA *psSysData,
					  IMG_UINT32 ui32DevIndex)
{
	DEVICE_COMMAND_DATA *psData;
	DEVICE_COMMAND_DATA *psRecheck;

	if (ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		return IMG_NULL;
	}
	psData = psSysData->apsDeviceCommandData[ui32DevIndex];
	if (psData == IMG_NULL)
	{
		return IMG_NULL;
	}
	atomic_inc(&sCmdProcRef[ui32DevIndex]);
	smp_mb();
	psRecheck = psSysData->apsDeviceCommandData[ui32DevIndex];
	if (psRecheck != psData)
	{
		smp_mb();
		atomic_dec(&sCmdProcRef[ui32DevIndex]);
		return IMG_NULL;
	}
	return psData;
}

static IMG_VOID CmdProcRefPut(IMG_UINT32 ui32DevIndex)
{
	if (ui32DevIndex < SYS_DEVICE_COUNT)
	{
		smp_mb();
		atomic_dec(&sCmdProcRef[ui32DevIndex]);
	}
}

/*
 * Cycle 7 admission pin (display-swap transactions only): same protocol as
 * CmdProcPinGet plus a lifecycle-state gate. The gate is validated before
 * and after the increment, with the increment fenced in between:
 *
 *   freeze: state = CLOSING; mb; wait refs == 0
 *   admit:  if (!table || state != RUNNING) return NULL;
 *           inc; mb; if (state != RUNNING || load(aps) != p) { mb; dec; return NULL; }
 *
 * If the admitter observes RUNNING, its increment is ordered before the
 * freezer's refs read (paired mb barriers), so the freezer drains it before
 * detaching; if the freezer's store is observed, the admitter unwinds without
 * ever touching the table. No new admission can begin after the freeze.
 */
static DEVICE_COMMAND_DATA *CmdProcAdmitGet(SYS_DATA *psSysData,
					    IMG_UINT32 ui32DevIndex)
{
	DEVICE_COMMAND_DATA *psData;

	if (ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		return IMG_NULL;
	}
	psData = psSysData->apsDeviceCommandData[ui32DevIndex];
	if (psData == IMG_NULL)
	{
		return IMG_NULL;
	}
	if (atomic_read(&sCmdProcState[ui32DevIndex]) != CMD_PROC_STATE_RUNNING)
	{
		return IMG_NULL;
	}
	atomic_inc(&sCmdProcRef[ui32DevIndex]);
	smp_mb();
	if (atomic_read(&sCmdProcState[ui32DevIndex]) != CMD_PROC_STATE_RUNNING ||
	    psSysData->apsDeviceCommandData[ui32DevIndex] != psData)
	{
		smp_mb();
		atomic_dec(&sCmdProcRef[ui32DevIndex]);
		return IMG_NULL;
	}
	return psData;
}


static IMG_UINT32 g_ui32OutStamp = 0;
static IMG_UINT32 g_ui32InStamp = 0;
//static IMG_HANDLE g_TimerHandle = IMG_NULL;


#if defined(__linux__) && defined(__KERNEL__)

#ifdef CONFIG_PVR_PROC
#include "proc.h"

/*****************************************************************************
 FUNCTION	:	ProcSeqShowQueue

 PURPOSE	:	Print the content of queue element  to /proc file
				(See env/linux/proc.c:CreateProcReadEntrySeq)

 PARAMETERS	:	sfile - /proc seq_file
				el - Element to print
*****************************************************************************/
void ProcSeqShowQueue(struct seq_file *sfile,void* el)
{
	PVRSRV_QUEUE_INFO *psQueue = (PVRSRV_QUEUE_INFO*)el;
	IMG_INT cmds = 0;
	IMG_SIZE_T uReadOffset;
	IMG_SIZE_T uWriteOffset;
	PVRSRV_COMMAND *psCmd;

	if(el == PVR_PROC_SEQ_START_TOKEN)
	{
		seq_printf( sfile,
					"Command Queues\n"
					"Queue    CmdPtr      Pid Command Size DevInd  DSC  SSC  #Data ...\n");
		return;
	}

	uReadOffset = psQueue->uReadOffset;
	uWriteOffset = psQueue->uWriteOffset;

	while (uReadOffset != uWriteOffset)
	{
		psCmd= (PVRSRV_COMMAND *)((IMG_UINTPTR_T)psQueue->pvLinQueueKM + uReadOffset);

		seq_printf(sfile, "%p %p  %5u  %6u  %3" SIZE_T_FMT_LEN "u  %5u   %2u   %2u    %3" SIZE_T_FMT_LEN "u  \n",
							psQueue,
							psCmd,
					 		psCmd->ui32ProcessID,
							psCmd->CommandType,
							psCmd->uCmdSize,
							psCmd->ui32DevIndex,
							psCmd->ui32DstSyncCount,
							psCmd->ui32SrcSyncCount,
							psCmd->uDataSize);
		{
			IMG_UINT32 i;
			for (i = 0; i < psCmd->ui32SrcSyncCount; i++)
			{
				PVRSRV_SYNC_DATA *psSyncData = psCmd->psSrcSync[i].psKernelSyncInfoKM->psSyncData;
				seq_printf(sfile, "  Sync %u: ROP/ROC: 0x%x/0x%x WOP/WOC: 0x%x/0x%x ROC-VA: 0x%x WOC-VA: 0x%x\n",
									i,
									psCmd->psSrcSync[i].ui32ReadOps2Pending,
									psSyncData->ui32ReadOps2Complete,
									psCmd->psSrcSync[i].ui32WriteOpsPending,
									psSyncData->ui32WriteOpsComplete,
									psCmd->psSrcSync[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
									psCmd->psSrcSync[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr);
			}
		}

		/* taken from UPDATE_QUEUE_ROFF in queue.h */
		uReadOffset += psCmd->uCmdSize;
		uReadOffset &= psQueue->uQueueSize - 1;
		cmds++;
	}

	if (cmds == 0)
	{
		seq_printf(sfile, "%p <empty>\n", psQueue);
	}
}

/*****************************************************************************
 FUNCTION	:	ProcSeqOff2ElementQueue

 PURPOSE	:	Transale offset to element (/proc stuff)

 PARAMETERS	:	sfile - /proc seq_file
				off - the offset into the buffer

 RETURNS    :   element to print
*****************************************************************************/
void* ProcSeqOff2ElementQueue(struct seq_file * sfile, loff_t off)
{
	PVRSRV_QUEUE_INFO *psQueue = IMG_NULL;
	SYS_DATA *psSysData;

	PVR_UNREFERENCED_PARAMETER(sfile);

	if(!off)
	{
		return PVR_PROC_SEQ_START_TOKEN;
	}


	psSysData = SysAcquireDataNoCheck();
	if (psSysData != IMG_NULL)
	{
		for (psQueue = psSysData->psQueueList; (((--off) > 0) && (psQueue != IMG_NULL)); psQueue = psQueue->psNextKM);
	}

	return psQueue;
}
#endif /* CONFIG_PVR_PROC */
#endif /* __linux__ && __KERNEL__ */

/*!
 * Macro to return space in given command queue
 */
#define GET_SPACE_IN_CMDQ(psQueue)										\
	((((psQueue)->uReadOffset - (psQueue)->uWriteOffset)				\
	+ ((psQueue)->uQueueSize - 1)) & ((psQueue)->uQueueSize - 1))

/*!
 * Macro to Write Offset in given command queue
 */
#define UPDATE_QUEUE_WOFF(psQueue, uSize)							\
	(psQueue)->uWriteOffset = ((psQueue)->uWriteOffset + (uSize))	\
	& ((psQueue)->uQueueSize - 1);

/*!
 * Check if an ops complete value has gone past the pending value.
 * This can happen when dummy processing multiple operations, e.g. hardware recovery.
 */
#define SYNCOPS_STALE(ui32OpsComplete, ui32OpsPending)					\
	((ui32OpsComplete) >= (ui32OpsPending))

/*!
****************************************************************************
 @Function	: PVRSRVGetWriteOpsPending

 @Description	: Gets the next operation to wait for in a sync object

 @Input	: psSyncInfo	- pointer to sync information struct
 @Input	: bIsReadOp		- Is this a read or write op

 @Return : Next op value
*****************************************************************************/
#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVGetWriteOpsPending)
#endif
static INLINE
IMG_UINT32 PVRSRVGetWriteOpsPending(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo, IMG_BOOL bIsReadOp)
{
	IMG_UINT32 ui32WriteOpsPending;

	if(bIsReadOp)
	{
		ui32WriteOpsPending = psSyncInfo->psSyncData->ui32WriteOpsPending;
	}
	else
	{
		/*
			Note: This needs to be atomic and is provided the
			kernel driver is single threaded (non-rentrant)
		*/
		ui32WriteOpsPending = SyncTakeWriteOp(psSyncInfo, SYNC_OP_CLASS_QUEUE);
	}

	return ui32WriteOpsPending;
}

/*!
*****************************************************************************
 @Function	: PVRSRVGetReadOpsPending

 @Description	: Gets the number of pending read ops

 @Input	: psSyncInfo	- pointer to sync information struct
 @Input : bIsReadOp		- Is this a read or write op

 @Return : Next op value
*****************************************************************************/
#ifdef INLINE_IS_PRAGMA
#pragma inline(PVRSRVGetReadOpsPending)
#endif
static INLINE
IMG_UINT32 PVRSRVGetReadOpsPending(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo, IMG_BOOL bIsReadOp)
{
	IMG_UINT32 ui32ReadOpsPending;

	if(bIsReadOp)
	{
		ui32ReadOpsPending = SyncTakeReadOp2(psSyncInfo, SYNC_OP_CLASS_QUEUE);
	}
	else
	{
		ui32ReadOpsPending = psSyncInfo->psSyncData->ui32ReadOps2Pending;
	}

	return ui32ReadOpsPending;
}

static IMG_VOID QueueDumpCmdComplete(COMMAND_COMPLETE_DATA *psCmdCompleteData,
									 IMG_UINT32				i,
									 IMG_BOOL				bIsSrc)
{
	PVRSRV_SYNC_OBJECT	*psSyncObject;

	psSyncObject = bIsSrc ? psCmdCompleteData->psSrcSync : psCmdCompleteData->psDstSync;

	if (psCmdCompleteData->bInUse)
	{
		PVR_LOG(("\t%s %u: ROC DevVAddr:0x%X ROP:0x%x ROC:0x%x, WOC DevVAddr:0x%X WOP:0x%x WOC:0x%x",
				bIsSrc ? "SRC" : "DEST", i,
				psSyncObject[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
				psSyncObject[i].psKernelSyncInfoKM->psSyncData->ui32ReadOps2Pending,
				psSyncObject[i].psKernelSyncInfoKM->psSyncData->ui32ReadOps2Complete,
				psSyncObject[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
				psSyncObject[i].psKernelSyncInfoKM->psSyncData->ui32WriteOpsPending,
				psSyncObject[i].psKernelSyncInfoKM->psSyncData->ui32WriteOpsComplete))
	}
	else
	{
		PVR_LOG(("\t%s %u: (Not in use)", bIsSrc ? "SRC" : "DEST", i))
	}
}


static IMG_VOID QueueDumpCommand(SYS_DATA *psSysData)
{
	PVRSRV_QUEUE_INFO	*psQueue;
	PVRSRV_COMMAND		*psCommand;
	PVRSRV_SYNC_OBJECT	*psSyncWalker;
	PVRSRV_SYNC_OBJECT	*psSyncEnd;
	PVRSRV_SYNC_DATA	*psSyncData;
	IMG_UINT32		start;

	psQueue = psSysData->psQueueList;

	while (psQueue)
	{
		start = psQueue->uReadOffset;

		PVR_LOG(("Queue Size:%d  ProcessID:0X%X", psQueue->uQueueSize, psQueue->ui32ProcessID));

		while (start != psQueue->uWriteOffset)
		{
			psCommand = (PVRSRV_COMMAND *)((unsigned int)psQueue->pvLinQueueKM + start);

			PVR_LOG(("CmdType:%d CmdSize:%d SrcCnt:%d DstCnt:%d", psCommand->CommandType,
				psCommand->uCmdSize, psCommand->ui32SrcSyncCount, psCommand->ui32DstSyncCount));

			psSyncWalker = psCommand->psDstSync;
			psSyncEnd = psSyncWalker + psCommand->ui32DstSyncCount;
			while (psSyncWalker < psSyncEnd)
			{
				psSyncData = psSyncWalker->psKernelSyncInfoKM->psSyncData;
				PVR_LOG(("\tDst Sync Object: Write[%X](%X:%X) Read[%X](%X:%X) Read2[%X](%X:%X) WP:%X, R2P:%X",
					psSyncWalker->psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
					psSyncData->ui32WriteOpsComplete, psSyncData->ui32WriteOpsPending,
					psSyncWalker->psKernelSyncInfoKM->sReadOpsCompleteDevVAddr.uiAddr,
					psSyncData->ui32ReadOpsComplete,  psSyncData->ui32ReadOpsPending,
					psSyncWalker->psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
					psSyncData->ui32ReadOps2Complete, psSyncData->ui32ReadOps2Pending,
					psSyncWalker->ui32WriteOpsPending, psSyncWalker->ui32ReadOps2Pending));
				psSyncWalker++;
			}

			psSyncWalker = psCommand->psSrcSync;
			psSyncEnd = psSyncWalker + psCommand->ui32SrcSyncCount;
			while (psSyncWalker < psSyncEnd)
			{
				psSyncData = psSyncWalker->psKernelSyncInfoKM->psSyncData;
				PVR_LOG(("\tSrc Sync Object: Write[%X](%X:%X) Read[%X](%X:%X) Read2[%X](%X:%X) WP:%X, R2P:%X",
					psSyncWalker->psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
					psSyncData->ui32WriteOpsComplete, psSyncData->ui32WriteOpsPending,
					psSyncWalker->psKernelSyncInfoKM->sReadOpsCompleteDevVAddr.uiAddr,
					psSyncData->ui32ReadOpsComplete,  psSyncData->ui32ReadOpsPending,
					psSyncWalker->psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
					psSyncData->ui32ReadOps2Complete, psSyncData->ui32ReadOps2Pending,
					psSyncWalker->ui32WriteOpsPending, psSyncWalker->ui32ReadOps2Pending));
				psSyncWalker++;
			}

			start += psCommand->uCmdSize;
			start &= (psQueue->uQueueSize - 1);
		}

		psQueue = psQueue->psNextKM;
	}
}


static IMG_VOID QueueDumpDebugInfo_ForEachCb(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	if (psDeviceNode->sDevId.eDeviceClass == PVRSRV_DEVICE_CLASS_DISPLAY)
	{
		IMG_UINT32				ui32CmdCounter, ui32SyncCounter;
		SYS_DATA				*psSysData;
		DEVICE_COMMAND_DATA		*psDeviceCommandData;
		PCOMMAND_COMPLETE_DATA	psCmdCompleteData;
		IMG_UINT32 i = 0, looper = 0;

		SysAcquireData(&psSysData);


		psDeviceCommandData = CmdProcPinGet(psSysData, psDeviceNode->sDevId.ui32DeviceIndex);
		PVR_LOG(("Current Jiffies: %lX", jiffies));

		if (psDeviceCommandData != IMG_NULL)
		{
			for (ui32CmdCounter = 0; ui32CmdCounter < DC_NUM_COMMANDS_PER_TYPE; ui32CmdCounter++)
			{
				psCmdCompleteData = psDeviceCommandData[DC_FLIP_COMMAND].apsCmdCompleteData[ui32CmdCounter];

				/* Cycle 5: also report the stored terminal status so a FAILED/
				 * ABORTED completion is visible to diagnostics (the status
				 * has no fence/userspace channel in this tree). Print-only;
				 * no behavioural change. */
				PVR_LOG(("Flip Command Complete Data %u for display device %u:; queuetime:%lX status:%d",
						ui32CmdCounter, psDeviceNode->sDevId.ui32DeviceIndex, psCmdCompleteData->ulQueueTime,
						(IMG_INT)psCmdCompleteData->eStatus))

				for (ui32SyncCounter = 0;
					 ui32SyncCounter < psCmdCompleteData->ui32SrcSyncCount;
					 ui32SyncCounter++)
				{
					QueueDumpCmdComplete(psCmdCompleteData, ui32SyncCounter, IMG_TRUE);
				}

				for (ui32SyncCounter = 0;
					 ui32SyncCounter < psCmdCompleteData->ui32DstSyncCount;
					 ui32SyncCounter++)
				{
					QueueDumpCmdComplete(psCmdCompleteData, ui32SyncCounter, IMG_FALSE);
				}
			}
			PVR_LOG(("Display complete history:"));
			looper = psDeviceCommandData->ui32THWriteOffset;
			for (i = 0; i < DC_NUM_CMDS_COMPLETE_HISTORY;) {
				looper = ((looper + 3) & ~3) % DC_NUM_CMDS_COMPLETE_HISTORY;
				printk(KERN_INFO "%08lX  %05lu          %08lX  %05lu",
					psDeviceCommandData->aulCompTimeHis[looper], psDeviceCommandData->aulCompTimeHis[looper+1],
					psDeviceCommandData->aulCompTimeHis[looper+2], psDeviceCommandData->aulCompTimeHis[looper+3]);
				looper += 4;
				if (looper >= DC_NUM_CMDS_COMPLETE_HISTORY)
					looper = 0;
				i += 4;
			}
		}
		else
		{
			PVR_LOG(("There is no Command Complete Data for display device %u", psDeviceNode->sDevId.ui32DeviceIndex))
		}

		/* Cycle 6: release only when Get returned a pinned table; a NULL
		 * return owns no reference. */
		if (psDeviceCommandData != IMG_NULL)
		{
			CmdProcRefPut(psDeviceNode->sDevId.ui32DeviceIndex);
		}
	}
}


IMG_VOID QueueDumpDebugInfo(IMG_VOID)
{
	SYS_DATA	*psSysData;
	SysAcquireData(&psSysData);
	QueueDumpCommand(psSysData);
	List_PVRSRV_DEVICE_NODE_ForEach(psSysData->psDeviceNodeList, &QueueDumpDebugInfo_ForEachCb);
}


/*****************************************************************************
	Kernel-side functions of User->Kernel transitions
******************************************************************************/

static IMG_SIZE_T NearestPower2(IMG_SIZE_T uValue)
{
	IMG_SIZE_T uTemp, uResult = 1;

	if(!uValue)
		return 0;

	uTemp = uValue - 1;
	while(uTemp)
	{
		uResult <<= 1;
		uTemp >>= 1;
	}

	return uResult;
}


/*!
******************************************************************************

 @Function	PVRSRVCreateCommandQueueKM

 @Description
 Creates a new command queue into which render/blt commands etc can be
 inserted.

 @Input    uQueueSize :

 @Output   ppsQueueInfo :

 @Return   PVRSRV_ERROR  :

******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVCreateCommandQueueKM(IMG_SIZE_T uQueueSize,
													 PVRSRV_QUEUE_INFO **ppsQueueInfo)
{
	PVRSRV_QUEUE_INFO	*psQueueInfo;
	IMG_SIZE_T			uPower2QueueSize = NearestPower2(uQueueSize);
	SYS_DATA			*psSysData;
	PVRSRV_ERROR		eError;
	IMG_HANDLE			hMemBlock;

	if (ui32NoOfSwapchainCreated >= DC_NUM_COMMANDS_PER_TYPE)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVCreateCommandQueueKM: Swapchain already exists, increament DC_MAX_SUPPORTED_QUEUES to support more than one swapchain"));
		return PVRSRV_ERROR_FLIP_CHAIN_EXISTS;
	}

	SysAcquireData(&psSysData);

	/* allocate an internal queue info structure */
	eError = OSAllocMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
					 sizeof(PVRSRV_QUEUE_INFO),
					 (IMG_VOID **)&psQueueInfo, &hMemBlock,
					 "Queue Info");
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVCreateCommandQueueKM: Failed to alloc queue struct"));
		goto ErrorExit;
	}
	OSMemSet(psQueueInfo, 0, sizeof(PVRSRV_QUEUE_INFO));

	psQueueInfo->hMemBlock[0] = hMemBlock;
	psQueueInfo->ui32ProcessID = OSGetCurrentProcessIDKM();

	/* allocate the command queue buffer - allow for overrun */
	eError = OSAllocMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
					 uPower2QueueSize + PVRSRV_MAX_CMD_SIZE,
					 &psQueueInfo->pvLinQueueKM, &hMemBlock,
					 "Command Queue");
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVCreateCommandQueueKM: Failed to alloc queue buffer"));
		goto ErrorExit;
	}

	psQueueInfo->hMemBlock[1] = hMemBlock;
	psQueueInfo->pvLinQueueUM = psQueueInfo->pvLinQueueKM;

	/* Sanity check: Should be zeroed by OSMemSet */
	PVR_ASSERT(psQueueInfo->uReadOffset == 0);
	PVR_ASSERT(psQueueInfo->uWriteOffset == 0);

	psQueueInfo->uQueueSize = uPower2QueueSize;

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	psQueueInfo->pvTimeline = sw_sync_timeline_create("pvr_queue_proc");
	if(psQueueInfo->pvTimeline == IMG_NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVCreateCommandQueueKM: sw_sync_timeline_create() failed"));
		goto ErrorExit;
	}
#endif

	/* if this is the first q, create a lock resource for the q list */
	if (psSysData->psQueueList == IMG_NULL)
	{
		eError = OSCreateResource(&psSysData->sQProcessResource);
		if (eError != PVRSRV_OK)
		{
			goto ErrorExit;
		}
	}

	/* Ensure we don't corrupt queue list, by blocking access */
	eError = OSLockResource(&psSysData->sQProcessResource,
							KERNEL_ID);
	if (eError != PVRSRV_OK)
	{
		goto ErrorExit;
	}

	psQueueInfo->psNextKM = psSysData->psQueueList;
	psSysData->psQueueList = psQueueInfo;

	eError = OSUnlockResource(&psSysData->sQProcessResource, KERNEL_ID);
	if (eError != PVRSRV_OK)
	{
		goto ErrorExit;
	}

	*ppsQueueInfo = psQueueInfo;

	ui32NoOfSwapchainCreated++;

	return PVRSRV_OK;

ErrorExit:

	if(psQueueInfo)
	{
		if(psQueueInfo->pvLinQueueKM)
		{
			OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
						psQueueInfo->uQueueSize,
						psQueueInfo->pvLinQueueKM,
						psQueueInfo->hMemBlock[1]);
			psQueueInfo->pvLinQueueKM = IMG_NULL;
		}

		OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
					sizeof(PVRSRV_QUEUE_INFO),
					psQueueInfo,
					psQueueInfo->hMemBlock[0]);
		/*not nulling pointer, out of scope*/
	}

	return eError;
}


/*!
******************************************************************************

 @Function	PVRSRVDestroyCommandQueueKM

 @Description	Destroys a command queue

 @Input		psQueueInfo :

 @Return	PVRSRV_ERROR

******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVDestroyCommandQueueKM(PVRSRV_QUEUE_INFO *psQueueInfo)
{
	PVRSRV_QUEUE_INFO	*psQueue;
	SYS_DATA			*psSysData;
	PVRSRV_ERROR		eError;
	IMG_BOOL			bTimeout = IMG_TRUE;

	SysAcquireData(&psSysData);

	psQueue = psSysData->psQueueList;

	/* PRQA S 3415,4109 1 */ /* macro format critical - leave alone */
	LOOP_UNTIL_TIMEOUT(MAX_HW_TIME_US)
	{
		if(psQueueInfo->uReadOffset == psQueueInfo->uWriteOffset)
		{
			bTimeout = IMG_FALSE;
			break;
		}
		/* Kick queue processing with flush=TRUE so pending commands drain */
		PVRSRVProcessQueues(IMG_TRUE);
		OSSleepms(1);
	} END_LOOP_UNTIL_TIMEOUT();

	if (bTimeout && (psQueueInfo->uReadOffset != psQueueInfo->uWriteOffset))
	{
		/* The queue could not be emptied within the timeout because GPU dependencies
		 * (ui32WriteOpsComplete < ui32WriteOpsPending) are still pending.
		 * We MUST NOT force-abort or drop meminfo references here: doing so would release
		 * backing physical pages while SGX hardware is still actively accessing/writing them.
		 * Instead, leave the queue and all meminfo references intact and return RETRY.
		 */
		PVR_DPF((PVR_DBG_WARNING, "PVRSRVDestroyCommandQueueKM: Queue not empty after drain timeout; retaining state"));
		return PVRSRV_ERROR_RETRY;
	}

	/* Ensure we don't corrupt queue list, by blocking access */
	eError = OSLockResource(&psSysData->sQProcessResource,
								KERNEL_ID);
	if (eError != PVRSRV_OK)
	{
		return PVRSRV_ERROR_RETRY;
	}

	ui32NoOfSwapchainCreated--;

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	sync_timeline_destroy(psQueueInfo->pvTimeline);
#endif

	if(psQueue == psQueueInfo)
	{
		psSysData->psQueueList = psQueueInfo->psNextKM;

		OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
					NearestPower2(psQueueInfo->uQueueSize) + PVRSRV_MAX_CMD_SIZE,
					psQueueInfo->pvLinQueueKM,
					psQueueInfo->hMemBlock[1]);
		psQueueInfo->pvLinQueueKM = IMG_NULL;
		OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
					sizeof(PVRSRV_QUEUE_INFO),
					psQueueInfo,
					psQueueInfo->hMemBlock[0]);
		/* PRQA S 3199 1 */ /* see note */
		psQueueInfo = IMG_NULL; /*it's a copy on stack, but null it because the function doesn't end right here*/
	}
	else
	{
		while(psQueue)
		{
			if(psQueue->psNextKM == psQueueInfo)
			{
				psQueue->psNextKM = psQueueInfo->psNextKM;

				OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
							psQueueInfo->uQueueSize,
							psQueueInfo->pvLinQueueKM,
							psQueueInfo->hMemBlock[1]);
				psQueueInfo->pvLinQueueKM = IMG_NULL;
				OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
							sizeof(PVRSRV_QUEUE_INFO),
							psQueueInfo,
							psQueueInfo->hMemBlock[0]);
				/* PRQA S 3199 1 */ /* see note */
				psQueueInfo = IMG_NULL; /*it's a copy on stack, but null it because the function doesn't end right here*/
				break;
			}
			psQueue = psQueue->psNextKM;
		}

		if(!psQueue)
		{
			eError = OSUnlockResource(&psSysData->sQProcessResource, KERNEL_ID);
			if (eError != PVRSRV_OK)
			{
				goto ErrorExit;
			}
			eError = PVRSRV_ERROR_INVALID_PARAMS;
			goto ErrorExit;
		}
	}

	/*  unlock the Q list lock resource */
	eError = OSUnlockResource(&psSysData->sQProcessResource, KERNEL_ID);
	if (eError != PVRSRV_OK)
	{
		goto ErrorExit;
	}

	/*  if the Q list is now empty, destroy the Q list lock resource */
	if (psSysData->psQueueList == IMG_NULL)
	{
		eError = OSDestroyResource(&psSysData->sQProcessResource);
		if (eError != PVRSRV_OK)
		{
			goto ErrorExit;
		}
	}

ErrorExit:

	return eError;
}


/*!
*****************************************************************************

 @Function	: PVRSRVGetQueueSpaceKM

 @Description	: Waits for queue access rights and checks for available space in
			  queue for task param structure

 @Input	: psQueue	   	- pointer to queue information struct
 @Input : ui32ParamSize - size of task data structure
 @Output : ppvSpace

 @Return	: PVRSRV_ERROR
*****************************************************************************/
IMG_EXPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVGetQueueSpaceKM(PVRSRV_QUEUE_INFO *psQueue,
												IMG_SIZE_T uParamSize,
												IMG_VOID **ppvSpace)
{
	/*	round to 4byte units */
	uParamSize =  (uParamSize + 3) & 0xFFFFFFFC;

	if (uParamSize > PVRSRV_MAX_CMD_SIZE)
	{
		PVR_DPF((PVR_DBG_WARNING,"PVRSRVGetQueueSpace: max command size is %d bytes", PVRSRV_MAX_CMD_SIZE));
		return PVRSRV_ERROR_CMD_TOO_BIG;
	}

	if (GET_SPACE_IN_CMDQ(psQueue) > uParamSize)
	{
		*ppvSpace = (IMG_VOID *)((IMG_UINTPTR_T)psQueue->pvLinQueueUM + psQueue->uWriteOffset);
	}
	else
	{
		*ppvSpace = IMG_NULL;
		return PVRSRV_ERROR_CANNOT_GET_QUEUE_SPACE;
	}

	return PVRSRV_OK;
}


/*!
*****************************************************************************
 @Function	PVRSRVInsertCommandKM

 @Description :
			command insertion utility
			 - waits for space in the queue for a new command
			 - fills in generic command information
			 - returns a pointer to the caller who's expected to then fill
			 	in the private data.
			The caller should follow PVRSRVInsertCommand with PVRSRVSubmitCommand
			which will update the queue's write offset so the command can be
			executed.

 @Input		psQueue : pointer to queue information struct

 @Output	ppvCmdData : holds pointer to space in queue for private cmd data

 @Return	PVRSRV_ERROR
*****************************************************************************/
IMG_EXPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVInsertCommandKM(PVRSRV_QUEUE_INFO	*psQueue,
												PVRSRV_COMMAND		**ppsCommand,
												IMG_UINT32			ui32DevIndex,
												IMG_UINT16			CommandType,
												IMG_UINT32			ui32DstSyncCount,
												PVRSRV_KERNEL_SYNC_INFO	*apsDstSync[],
												IMG_UINT32			ui32SrcSyncCount,
												PVRSRV_KERNEL_SYNC_INFO	*apsSrcSync[],
												IMG_SIZE_T			uDataByteSize,
												PFN_QUEUE_COMMAND_COMPLETE pfnCommandComplete,
												IMG_HANDLE			hCallbackData,
												IMG_HANDLE			*phFence)
{
	PVRSRV_ERROR 	eError;
	PVRSRV_COMMAND	*psCommand;
	IMG_SIZE_T		uCommandSize;
	IMG_UINT32		i;
	SYS_DATA *psSysData;
	DEVICE_COMMAND_DATA *psDeviceCommandData;

#if !defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	PVR_UNREFERENCED_PARAMETER(phFence);
#endif

	/* Check that we've got enough space in our command complete data for this command */
	SysAcquireData(&psSysData);
	psDeviceCommandData = CmdProcPinGet(psSysData, ui32DevIndex);

	/* Cycle 5/6: the command-processor table may have been detached by a
	 * concurrent removal (DeInit). Fail closed instead of dereferencing
	 * NULL. A NULL return owns no reference (Cycle 6 semantics A), so only
	 * the validated path below holds a pin, released right after. */
	if (psDeviceCommandData == IMG_NULL)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVInsertCommandKM: cmdproc detached"));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	if ((psDeviceCommandData[CommandType].ui32MaxDstSyncCount < ui32DstSyncCount) ||
	   (psDeviceCommandData[CommandType].ui32MaxSrcSyncCount < ui32SrcSyncCount))
	{
		CmdProcRefPut(ui32DevIndex);
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVInsertCommandKM: Too many syncs"));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	CmdProcRefPut(ui32DevIndex);

	/* Round up to nearest 32 bit size so pointer arithmetic works */
	uDataByteSize = (uDataByteSize + 3UL) & ~3UL;

	/*  calc. command size */
	uCommandSize = sizeof(PVRSRV_COMMAND)
					+ ((ui32DstSyncCount + ui32SrcSyncCount) * sizeof(PVRSRV_SYNC_OBJECT))
					+ uDataByteSize;

	/* wait for space in queue */
	eError = PVRSRVGetQueueSpaceKM (psQueue, uCommandSize, (IMG_VOID**)&psCommand);
	if(eError != PVRSRV_OK)
	{
		return eError;
	}

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	if(phFence != IMG_NULL)
	{
		struct sync_fence *psRetireFence, *psCleanupFence;

		/* New command? New timeline target */
		psQueue->ui32FenceValue++;

		psRetireFence = AllocQueueFence(psQueue->pvTimeline, psQueue->ui32FenceValue, "pvr_queue_retire");
		if(!psRetireFence)
		{
			PVR_DPF((PVR_DBG_ERROR, "PVRSRVInsertCommandKM: sync_fence_create() failed"));
			psQueue->ui32FenceValue--;
			return PVRSRV_ERROR_INVALID_PARAMS;
		}

		/* This similar to the retire fence, except that it is destroyed
		 * when a display command completes, rather than at the whim of
		 * userspace. It is used to keep the timeline alive.
		 */
		psCleanupFence = AllocQueueFence(psQueue->pvTimeline, psQueue->ui32FenceValue, "pvr_queue_cleanup");
		if(!psCleanupFence)
		{
			PVR_DPF((PVR_DBG_ERROR, "PVRSRVInsertCommandKM: sync_fence_create() #2 failed"));
			sync_fence_put(psRetireFence);
			psQueue->ui32FenceValue--;
			return PVRSRV_ERROR_INVALID_PARAMS;
		}

		psCommand->pvCleanupFence = psCleanupFence;
		psCommand->pvTimeline = psQueue->pvTimeline;
		*phFence = psRetireFence;
	}
	else
	{
		psCommand->pvTimeline = IMG_NULL;
	}
#endif /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC) */

	psCommand->ui32ProcessID	= OSGetCurrentProcessIDKM();

	/* setup the command */
	psCommand->uCmdSize			= uCommandSize; /* this may change if cmd shrinks */
	psCommand->ui32DevIndex 	= ui32DevIndex;
	psCommand->CommandType 		= CommandType;
	psCommand->ui32DstSyncCount	= ui32DstSyncCount;
	psCommand->ui32SrcSyncCount	= ui32SrcSyncCount;
	/* override QAC warning about stricter pointers */
	/* PRQA S 3305 END_PTR_ASSIGNMENTS */
	psCommand->psDstSync		= (PVRSRV_SYNC_OBJECT*)(((IMG_UINTPTR_T)psCommand) + sizeof(PVRSRV_COMMAND));


	psCommand->psSrcSync		= (PVRSRV_SYNC_OBJECT*)(((IMG_UINTPTR_T)psCommand->psDstSync)
								+ (ui32DstSyncCount * sizeof(PVRSRV_SYNC_OBJECT)));

	psCommand->pvData			= (PVRSRV_SYNC_OBJECT*)(((IMG_UINTPTR_T)psCommand->psSrcSync)
								+ (ui32SrcSyncCount * sizeof(PVRSRV_SYNC_OBJECT)));
/* PRQA L:END_PTR_ASSIGNMENTS */

	psCommand->uDataSize		= uDataByteSize;/* this may change if cmd shrinks */

	psCommand->pfnCommandComplete = pfnCommandComplete;
	psCommand->hCallbackData = hCallbackData;

	PVR_TTRACE(PVRSRV_TRACE_GROUP_QUEUE, PVRSRV_TRACE_CLASS_CMD_START, QUEUE_TOKEN_INSERTKM);
	PVR_TTRACE_UI32(PVRSRV_TRACE_GROUP_QUEUE, PVRSRV_TRACE_CLASS_NONE,
			QUEUE_TOKEN_COMMAND_TYPE, CommandType);

	/* setup dst sync objects and their sync dependencies */
	for (i=0; i<ui32DstSyncCount; i++)
	{
		PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_QUEUE, QUEUE_TOKEN_DST_SYNC,
						apsDstSync[i], PVRSRV_SYNCOP_SAMPLE);

		psCommand->psDstSync[i].psKernelSyncInfoKM = apsDstSync[i];
		psCommand->psDstSync[i].ui32WriteOpsPending = PVRSRVGetWriteOpsPending(apsDstSync[i], IMG_FALSE);
		psCommand->psDstSync[i].ui32ReadOps2Pending = PVRSRVGetReadOpsPending(apsDstSync[i], IMG_FALSE);

		PVRSRVKernelSyncInfoIncRef(apsDstSync[i], IMG_NULL);

		PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVInsertCommandKM: Dst %u RO-VA:0x%x WO-VA:0x%x ROP:0x%x WOP:0x%x",
				i, psCommand->psDstSync[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
				psCommand->psDstSync[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
				psCommand->psDstSync[i].ui32ReadOps2Pending,
				psCommand->psDstSync[i].ui32WriteOpsPending));
	}

	/* setup src sync objects and their sync dependencies */
	for (i=0; i<ui32SrcSyncCount; i++)
	{
		PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_QUEUE, QUEUE_TOKEN_DST_SYNC,
						apsSrcSync[i], PVRSRV_SYNCOP_SAMPLE);

		psCommand->psSrcSync[i].psKernelSyncInfoKM = apsSrcSync[i];
		psCommand->psSrcSync[i].ui32WriteOpsPending = PVRSRVGetWriteOpsPending(apsSrcSync[i], IMG_TRUE);
		psCommand->psSrcSync[i].ui32ReadOps2Pending = PVRSRVGetReadOpsPending(apsSrcSync[i], IMG_TRUE);

		PVRSRVKernelSyncInfoIncRef(apsSrcSync[i], IMG_NULL);

		PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVInsertCommandKM: Src %u RO-VA:0x%x WO-VA:0x%x ROP:0x%x WOP:0x%x",
				i, psCommand->psSrcSync[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
				psCommand->psSrcSync[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
				psCommand->psSrcSync[i].ui32ReadOps2Pending,
				psCommand->psSrcSync[i].ui32WriteOpsPending));
	}
	PVR_TTRACE(PVRSRV_TRACE_GROUP_QUEUE, PVRSRV_TRACE_CLASS_CMD_END, QUEUE_TOKEN_INSERTKM);

	/* return pointer to caller to fill out private data */
	*ppsCommand = psCommand;

	return PVRSRV_OK;
}


/*!
*******************************************************************************
 @Function	: PVRSRVSubmitCommandKM

 @Description :
 			 updates the queue's write offset so the command can be executed.

 @Input	: psQueue 		-	queue command is in
 @Input	: psCommand

 @Return : PVRSRV_ERROR
******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVSubmitCommandKM(PVRSRV_QUEUE_INFO *psQueue,
												PVRSRV_COMMAND *psCommand)
{
	/* override QAC warnings about stricter pointers */
	/* PRQA S 3305 END_PTR_ASSIGNMENTS2 */
	/* patch pointers in the command to be kernel pointers */
	if (psCommand->ui32DstSyncCount > 0)
	{
		psCommand->psDstSync = (PVRSRV_SYNC_OBJECT*)(((IMG_UINTPTR_T)psQueue->pvLinQueueKM)
									+ psQueue->uWriteOffset + sizeof(PVRSRV_COMMAND));
	}

	if (psCommand->ui32SrcSyncCount > 0)
	{
		psCommand->psSrcSync = (PVRSRV_SYNC_OBJECT*)(((IMG_UINTPTR_T)psQueue->pvLinQueueKM)
									+ psQueue->uWriteOffset + sizeof(PVRSRV_COMMAND)
									+ (psCommand->ui32DstSyncCount * sizeof(PVRSRV_SYNC_OBJECT)));
	}

	psCommand->pvData = (PVRSRV_SYNC_OBJECT*)(((IMG_UINTPTR_T)psQueue->pvLinQueueKM)
									+ psQueue->uWriteOffset + sizeof(PVRSRV_COMMAND)
									+ (psCommand->ui32DstSyncCount * sizeof(PVRSRV_SYNC_OBJECT))
									+ (psCommand->ui32SrcSyncCount * sizeof(PVRSRV_SYNC_OBJECT)));

/* PRQA L:END_PTR_ASSIGNMENTS2 */

	/*
	 * Cycle 7: take command-processor ownership for this command before the
	 * write offset publishes it, so a concurrent detach that observes the
	 * queue (or the owner count) can never miss it. The matching release is
	 * the single terminal-pop site in PVRSRVProcessQueues. Callers of this
	 * function hold an admission pin across the submit (deviceclass.c), so
	 * the freeze/drain protocol covers this increment as well.
	 */
	if (psCommand->ui32DevIndex < SYS_DEVICE_COUNT)
	{
		atomic_inc(&sCmdProcQueued[psCommand->ui32DevIndex]);
		smp_mb();
	}

	/* update write offset before releasing access lock */
	UPDATE_QUEUE_WOFF(psQueue, psCommand->uCmdSize);

	return PVRSRV_OK;
}

/*!
******************************************************************************

 @Function	CheckIfSyncIsQueued

 @Description	Check if the specificed sync object is already queued and
                can safely be given to the display controller.
                This check is required as a 3rd party displayclass device can
                have several flips "in flight" and we need to ensure that we
                keep their pipeline full and don't deadlock waiting for them
                to complete an operation on a surface.

 @Input		psSysData : system data
 @Input		psCmdData : COMMAND_COMPLETE_DATA structure

 @Return	PVRSRV_ERROR

******************************************************************************/
static
PVRSRV_ERROR CheckIfSyncIsQueued(PVRSRV_SYNC_OBJECT *psSync, COMMAND_COMPLETE_DATA *psCmdData)
{
	IMG_UINT32 k;
 
	if (psCmdData->bInUse)
	{
		for (k=0;k<psCmdData->ui32SrcSyncCount;k++)
		{
			if (psSync->psKernelSyncInfoKM == psCmdData->psSrcSync[k].psKernelSyncInfoKM)
			{
				PVRSRV_SYNC_DATA *psSyncData = psSync->psKernelSyncInfoKM->psSyncData;
				IMG_UINT32 ui32WriteOpsComplete = psSyncData->ui32WriteOpsComplete;

				/*
					We still need to ensure that we don't we don't give a command
					to the display controller if writes are outstanding on it
				*/
				if (ui32WriteOpsComplete == psSync->ui32WriteOpsPending)
				{
					return PVRSRV_OK;
				}
				else
				{
					if (SYNCOPS_STALE(ui32WriteOpsComplete, psSync->ui32WriteOpsPending))
					{
						PVR_DPF((PVR_DBG_WARNING,
								"CheckIfSyncIsQueued: Stale syncops psSyncData:0x%p ui32WriteOpsComplete:0x%x ui32WriteOpsPending:0x%x",
								psSyncData, ui32WriteOpsComplete, psSync->ui32WriteOpsPending));
						return PVRSRV_OK;
					}
				}
			}
		}
	}
	return PVRSRV_ERROR_FAILED_DEPENDENCIES;
}

/*!
******************************************************************************

 @Function	PVRSRVProcessCommand

 @Description	Tries to process a command

 @Input		psSysData : system data
 @Input		psCommand : PVRSRV_COMMAND structure
 @Input		bFlush : Check for stale dependencies (only used for HW recovery)

 @Return	PVRSRV_ERROR

******************************************************************************/
static
PVRSRV_ERROR PVRSRVProcessCommand(SYS_DATA			*psSysData,
								  PVRSRV_COMMAND	*psCommand,
								  IMG_BOOL			bFlush)
{
	PVRSRV_SYNC_OBJECT		*psWalkerObj;
	PVRSRV_SYNC_OBJECT		*psEndObj;
	IMG_UINT32				i;
	COMMAND_COMPLETE_DATA	*psCmdCompleteData;
	PVRSRV_ERROR			eError = PVRSRV_OK;
	IMG_UINT32				ui32WriteOpsComplete;
	IMG_UINT32				ui32ReadOpsComplete;
	DEVICE_COMMAND_DATA		*psDeviceCommandData;
	IMG_UINT32				ui32CCBOffset;

	/* satisfy sync dependencies on the DST(s) */
	psWalkerObj = psCommand->psDstSync;
	psEndObj = psWalkerObj + psCommand->ui32DstSyncCount;
	while (psWalkerObj < psEndObj)
	{
		PVRSRV_SYNC_DATA *psSyncData = psWalkerObj->psKernelSyncInfoKM->psSyncData;

		ui32WriteOpsComplete = psSyncData->ui32WriteOpsComplete;
		ui32ReadOpsComplete = psSyncData->ui32ReadOps2Complete;
		/* fail if reads or writes are not up to date */
		if ((ui32WriteOpsComplete != psWalkerObj->ui32WriteOpsPending)
		||	(ui32ReadOpsComplete != psWalkerObj->ui32ReadOps2Pending))
		{
			if (!bFlush ||
				!SYNCOPS_STALE(ui32WriteOpsComplete, psWalkerObj->ui32WriteOpsPending) ||
				!SYNCOPS_STALE(ui32ReadOpsComplete, psWalkerObj->ui32ReadOps2Pending))
			{
				return PVRSRV_ERROR_FAILED_DEPENDENCIES;
			}
		}

		psWalkerObj++;
	}

	/* satisfy sync dependencies on the SRC(s) */
	psWalkerObj = psCommand->psSrcSync;
	psEndObj = psWalkerObj + psCommand->ui32SrcSyncCount;
	while (psWalkerObj < psEndObj)
	{
		PVRSRV_SYNC_DATA *psSyncData = psWalkerObj->psKernelSyncInfoKM->psSyncData;

		ui32ReadOpsComplete = psSyncData->ui32ReadOps2Complete;
		ui32WriteOpsComplete = psSyncData->ui32WriteOpsComplete;
		/* fail if writes are not up to date */
		if ((ui32WriteOpsComplete != psWalkerObj->ui32WriteOpsPending)
		|| (ui32ReadOpsComplete != psWalkerObj->ui32ReadOps2Pending))
		{
			if (!bFlush &&
				SYNCOPS_STALE(ui32WriteOpsComplete, psWalkerObj->ui32WriteOpsPending) &&
				SYNCOPS_STALE(ui32ReadOpsComplete, psWalkerObj->ui32ReadOps2Pending))
			{
				PVR_DPF((PVR_DBG_WARNING,
						"PVRSRVProcessCommand: Stale syncops psSyncData:0x%p ui32WriteOpsComplete:0x%x ui32WriteOpsPending:0x%x",
						psSyncData, ui32WriteOpsComplete, psWalkerObj->ui32WriteOpsPending));
			}

			if (!bFlush ||
				!SYNCOPS_STALE(ui32WriteOpsComplete, psWalkerObj->ui32WriteOpsPending) ||
				!SYNCOPS_STALE(ui32ReadOpsComplete, psWalkerObj->ui32ReadOps2Pending))
			{
				IMG_UINT32 j;
				PVRSRV_ERROR eError;
				IMG_BOOL bFound = IMG_FALSE;

				psDeviceCommandData = psSysData->apsDeviceCommandData[psCommand->ui32DevIndex];
				/* Cycle 4: the table may have been detached by a concurrent
				 * removal (DeInit races queue drain). Same fail-closed
				 * error as the post-validation check below; the command
				 * stays queued and the drain path retries. */
				if (psDeviceCommandData == IMG_NULL)
				{
					return PVRSRV_ERROR_INVALID_PARAMS;
				}
				for (j=0;j<DC_NUM_COMMANDS_PER_TYPE;j++)
				{
					eError = CheckIfSyncIsQueued(psWalkerObj, psDeviceCommandData[psCommand->CommandType].apsCmdCompleteData[j]);

					if (eError == PVRSRV_OK)
					{
						bFound = IMG_TRUE;
					}
				}
				if (!bFound)
					return PVRSRV_ERROR_FAILED_DEPENDENCIES;
			}
		}
		psWalkerObj++;
	}

	/* validate device type */
	if (psCommand->ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		PVR_DPF((PVR_DBG_ERROR,
					"PVRSRVProcessCommand: invalid DeviceType 0x%x",
					psCommand->ui32DevIndex));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* fish out the appropriate storage structure for the duration of the command */
	psDeviceCommandData = psSysData->apsDeviceCommandData[psCommand->ui32DevIndex];
	if (psDeviceCommandData == IMG_NULL)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	ui32CCBOffset = psDeviceCommandData[psCommand->CommandType].ui32CCBOffset;
	psCmdCompleteData = psDeviceCommandData[psCommand->CommandType].apsCmdCompleteData[ui32CCBOffset];
	if (psCmdCompleteData->bInUse)
	{
		/* can use this to protect against concurrent execution of same command */
		return PVRSRV_ERROR_FAILED_DEPENDENCIES;
	}

	/* mark the structure as in use */
	psCmdCompleteData->bInUse = IMG_TRUE;

	/* copy src updates over */
	psCmdCompleteData->ui32DstSyncCount = psCommand->ui32DstSyncCount;
	for (i=0; i<psCommand->ui32DstSyncCount; i++)
	{
		psCmdCompleteData->psDstSync[i] = psCommand->psDstSync[i];

		PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVProcessCommand: Dst %u RO-VA:0x%x WO-VA:0x%x ROP:0x%x WOP:0x%x (CCB:%u)",
				i, psCmdCompleteData->psDstSync[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
				psCmdCompleteData->psDstSync[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
				psCmdCompleteData->psDstSync[i].ui32ReadOps2Pending,
				psCmdCompleteData->psDstSync[i].ui32WriteOpsPending,
				ui32CCBOffset));
	}

	psCmdCompleteData->pfnCommandComplete = psCommand->pfnCommandComplete;
	psCmdCompleteData->hCallbackData = psCommand->hCallbackData;

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	psCmdCompleteData->pvCleanupFence = psCommand->pvCleanupFence;
	psCmdCompleteData->pvTimeline = psCommand->pvTimeline;
#endif

	/* copy dst updates over */
	psCmdCompleteData->ui32SrcSyncCount = psCommand->ui32SrcSyncCount;
	for (i=0; i<psCommand->ui32SrcSyncCount; i++)
	{
		psCmdCompleteData->psSrcSync[i] = psCommand->psSrcSync[i];

		PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVProcessCommand: Src %u RO-VA:0x%x WO-VA:0x%x ROP:0x%x WOP:0x%x (CCB:%u)",
				i, psCmdCompleteData->psSrcSync[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
				psCmdCompleteData->psSrcSync[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
				psCmdCompleteData->psSrcSync[i].ui32ReadOps2Pending,
				psCmdCompleteData->psSrcSync[i].ui32WriteOpsPending,
				ui32CCBOffset));
	}

	psCmdCompleteData->ui32Stamp = g_ui32OutStamp++;
	psCmdCompleteData->ulQueueTime = jiffies;
	psCmdCompleteData->ui32DevIndex = psCommand->ui32DevIndex;

	/*
		call the cmd specific handler:
		it should:
		 - check the cmd specific dependencies
		 - setup private cmd complete structure
		 - execute cmd on HW
		 - store psCmdCompleteData `cookie' and later pass as
			argument to Generic Command Complete Callback

		n.b. ui32DataSize (packet size) is useful for packet validation
	*/
	if (psDeviceCommandData[psCommand->CommandType].pfnCmdProc((IMG_HANDLE)psCmdCompleteData,
															   (IMG_UINT32)psCommand->uDataSize,
															   psCommand->pvData, bFlush) == IMG_FALSE)
	{
		/*
			clean-up:
			free cmd complete structure
		*/
		g_ui32InStamp++;
		psCmdCompleteData->bInUse = IMG_FALSE;
		eError = PVRSRV_ERROR_CMD_NOT_PROCESSED;
		PVR_LOG(("Failed to submit command(0x%x) from queue processor, this could cause sync wedge!", psCommand->ui32DevIndex));
	} else {
		/* Increment the CCB offset */
		psDeviceCommandData[psCommand->CommandType].ui32CCBOffset = (ui32CCBOffset + 1) % DC_NUM_COMMANDS_PER_TYPE;
	}
/*temporarily disable the timer as we have flip timer and they have race now*/
#if 0
	if ((g_ui32OutStamp - g_ui32InStamp) == DC_NUM_COMMANDS_PER_TYPE)
	{
		/*
			We've just sent out a new flip which has filled the DC's pipeline.
			This means that we expect a complete within a VSync period, start
			a timer that will print out a message if we haven't got a complete
			within a reasonable period (200ms)
		*/
		if (g_TimerHandle != IMG_NULL) {
			PVR_DPF((PVR_DBG_ERROR, "service queue debug timer is already in use"));
		} else {
			g_TimerHandle = OSAddTimer(_CommandCompleteTimeout, psCmdCompleteData, 200);
			OSEnableTimer(g_TimerHandle);
		}
	}
#endif

	return eError;
}


static IMG_VOID PVRSRVProcessQueues_ForEachCb(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	if (psDeviceNode->bReProcessDeviceCommandComplete &&
		psDeviceNode->pfnDeviceCommandComplete != IMG_NULL)
	{
		(*psDeviceNode->pfnDeviceCommandComplete)(psDeviceNode);
	}
}

/*!
******************************************************************************

 @Function	PVRSRVProcessQueues

 @Description	Tries to process a command from each Q

 @input ui32CallerID - used to distinguish between async ISR/DPC type calls
 						the synchronous services driver
 @input	bFlush - flush commands with stale dependencies (only used for HW recovery)

 @Return	PVRSRV_ERROR

******************************************************************************/

IMG_EXPORT
PVRSRV_ERROR PVRSRVProcessQueues(IMG_BOOL	bFlush)
{
	PVRSRV_QUEUE_INFO 	*psQueue;
	SYS_DATA			*psSysData;
	PVRSRV_COMMAND 		*psCommand;
/*	PVRSRV_DEVICE_NODE	*psDeviceNode;*/

	SysAcquireData(&psSysData);

	/* Ensure we don't corrupt queue list, by blocking access. This is required for OSs where
	    multiple ISR threads may exist simultaneously (eg WinXP DPC routines)
	*/
	while (OSLockResource(&psSysData->sQProcessResource, ISR_ID) != PVRSRV_OK)
	{
		OSWaitus(1);
	};
	
	psQueue = psSysData->psQueueList;

	if(!psQueue)
	{
		PVR_DPF((PVR_DBG_MESSAGE,"No Queues installed - cannot process commands"));
	}

	if (bFlush)
	{
		PVRSRVSetDCState(DC_STATE_FLUSH_COMMANDS);
	}

	while (psQueue)
	{
		while (psQueue->uReadOffset != psQueue->uWriteOffset)
		{
			IMG_UINT32 ui32QueuedDev;

			psCommand = (PVRSRV_COMMAND*)((IMG_UINTPTR_T)psQueue->pvLinQueueKM + psQueue->uReadOffset);
			/* Capture before the command can be recycled by a later submit. */
			ui32QueuedDev = psCommand->ui32DevIndex;

			if (PVRSRVProcessCommand(psSysData, psCommand, bFlush) == PVRSRV_OK)
			{
				/* processed cmd so update queue */
				UPDATE_QUEUE_ROFF(psQueue, psCommand->uCmdSize)

				/*
				 * Cycle 7: terminal pop releases the command-processor
				 * ownership taken in PVRSRVSubmitCommandKM. This is the
				 * only release site; retried commands (ProcessCommand
				 * error) keep their ownership and are re-visited.
				 */
				if (ui32QueuedDev < SYS_DEVICE_COUNT)
				{
					smp_mb();
					atomic_dec(&sCmdProcQueued[ui32QueuedDev]);
				}
				continue;
			}

			break;
		}
		psQueue = psQueue->psNextKM;
	}

	if (bFlush)
	{
		PVRSRVSetDCState(DC_STATE_NO_FLUSH_COMMANDS);
	}

	/* Re-process command complete handlers if necessary. */
	List_PVRSRV_DEVICE_NODE_ForEach(psSysData->psDeviceNodeList,
									&PVRSRVProcessQueues_ForEachCb);

	OSUnlockResource(&psSysData->sQProcessResource, ISR_ID);

	return PVRSRV_OK;
}


/*!
******************************************************************************

 @Function	PVRSRVCommandCompleteKM

 @Description	Updates non-private command complete sync objects

 @Input		hCmdCookie : command cookie
 @Input		bScheduleMISR : boolean to schedule MISR

 @Return	PVRSRV_ERROR

******************************************************************************/
IMG_EXPORT
IMG_VOID PVRSRVCommandCompleteStatusKM(IMG_HANDLE	hCmdCookie,
									   IMG_BOOL		bScheduleMISR,
									   PVRSRV_COMMAND_STATUS eStatus)
{
	IMG_UINT32				i;
	COMMAND_COMPLETE_DATA	*psCmdCompleteData = (COMMAND_COMPLETE_DATA *)hCmdCookie;
	SYS_DATA				*psSysData;
	DEVICE_COMMAND_DATA		*psDeviceCommandData;
	unsigned long time = 0;
	IMG_UINT32				ui32DevIndex;
	IMG_BOOL				bPinned = IMG_FALSE;

	SysAcquireData(&psSysData);

	ui32DevIndex = psCmdCompleteData->ui32DevIndex;
	/* Cycle 4/6: pin the command-processor table across this completion so a
	 * concurrent detach+free cannot release the history storage
	 * (aulCompTimeHis) used below. The cookie itself stays covered by the
	 * exactly-once/bInUse protocol. Cycle 6: the pin exists only when Get
	 * returns non-NULL; every Put below is guarded by bPinned. */
	bPinned = (CmdProcPinGet(psSysData, ui32DevIndex) != IMG_NULL);

#if 0
	if (g_TimerHandle)
	{
		OSDisableTimer(g_TimerHandle);
		OSRemoveTimer(g_TimerHandle);
		g_TimerHandle = IMG_NULL;
	}
#endif

	if (psCmdCompleteData->bInUse  != IMG_TRUE)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVCommandCompleteStatusKM: complete on already completed CCD %p",
				psCmdCompleteData));
		if (bPinned)
		{
			CmdProcRefPut(ui32DevIndex);
		}
		return;
	}

	psDeviceCommandData = psSysData->apsDeviceCommandData[psCmdCompleteData->ui32DevIndex];

	if (psCmdCompleteData->ui32Stamp != g_ui32InStamp)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVCommandCompleteStatusKM: Complete arrived in unexpected order (got %d expecting %d)",
				psCmdCompleteData->ui32Stamp,
				g_ui32InStamp));
	}

	g_ui32InStamp++;

	/* Record command completion status */
	psCmdCompleteData->eStatus = eStatus;

	PVR_TTRACE(PVRSRV_TRACE_GROUP_QUEUE, PVRSRV_TRACE_CLASS_CMD_COMP_START,
			QUEUE_TOKEN_COMMAND_COMPLETE);

	/* update DST(s) syncs */
	for (i=0; i<psCmdCompleteData->ui32DstSyncCount; i++)
	{
		psCmdCompleteData->psDstSync[i].psKernelSyncInfoKM->psSyncData->ui32WriteOpsComplete++;

		PVRSRVKernelSyncInfoDecRef(psCmdCompleteData->psDstSync[i].psKernelSyncInfoKM, IMG_NULL);

		PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_QUEUE, QUEUE_TOKEN_UPDATE_DST,
					  psCmdCompleteData->psDstSync[i].psKernelSyncInfoKM,
					  PVRSRV_SYNCOP_COMPLETE);

		PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVCommandCompleteKM: Dst %u RO-VA:0x%x WO-VA:0x%x ROP:0x%x WOP:0x%x",
				i, psCmdCompleteData->psDstSync[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
				psCmdCompleteData->psDstSync[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
				psCmdCompleteData->psDstSync[i].ui32ReadOps2Pending,
				psCmdCompleteData->psDstSync[i].ui32WriteOpsPending));
	}

	/* update SRC(s) syncs */
	for (i=0; i<psCmdCompleteData->ui32SrcSyncCount; i++)
	{
		psCmdCompleteData->psSrcSync[i].psKernelSyncInfoKM->psSyncData->ui32ReadOps2Complete++;

		PVRSRVKernelSyncInfoDecRef(psCmdCompleteData->psSrcSync[i].psKernelSyncInfoKM, IMG_NULL);

		PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_QUEUE, QUEUE_TOKEN_UPDATE_SRC,
					  psCmdCompleteData->psSrcSync[i].psKernelSyncInfoKM,
					  PVRSRV_SYNCOP_COMPLETE);

		PVR_DPF((PVR_DBG_MESSAGE, "PVRSRVCommandCompleteKM: Src %u RO-VA:0x%x WO-VA:0x%x ROP:0x%x WOP:0x%x",
				i, psCmdCompleteData->psSrcSync[i].psKernelSyncInfoKM->sReadOps2CompleteDevVAddr.uiAddr,
				psCmdCompleteData->psSrcSync[i].psKernelSyncInfoKM->sWriteOpsCompleteDevVAddr.uiAddr,
				psCmdCompleteData->psSrcSync[i].ui32ReadOps2Pending,
				psCmdCompleteData->psSrcSync[i].ui32WriteOpsPending));
	}

	PVR_TTRACE(PVRSRV_TRACE_GROUP_QUEUE, PVRSRV_TRACE_CLASS_CMD_COMP_END,
			QUEUE_TOKEN_COMMAND_COMPLETE);

	if (psCmdCompleteData->pfnCommandComplete)
	{
		psCmdCompleteData->pfnCommandComplete(psCmdCompleteData->hCallbackData);
	}
	/*save complete time history if device command data is available*/
	if (psDeviceCommandData != IMG_NULL)
	{
		time = jiffies - psCmdCompleteData->ulQueueTime;
		if (jiffies_to_msecs(time) > 35) {
			psDeviceCommandData->aulCompTimeHis[psDeviceCommandData->ui32THWriteOffset++] =
				psCmdCompleteData->ulQueueTime;
			psDeviceCommandData->aulCompTimeHis[psDeviceCommandData->ui32THWriteOffset++] = time;
			if (psDeviceCommandData->ui32THWriteOffset >= DC_NUM_CMDS_COMPLETE_HISTORY)
				psDeviceCommandData->ui32THWriteOffset = 0;
		}
	}

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	if(psCmdCompleteData->pvTimeline)
	{
		/* Only advance timeline as presented on actual presentation success */
		if (eStatus == PVRSRV_COMMAND_STATUS_PRESENTED)
		{
			sw_sync_timeline_inc(psCmdCompleteData->pvTimeline, 1);
		}
		sync_fence_put(psCmdCompleteData->pvCleanupFence);
	}
#endif /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC) */

	/* free command complete storage */
	psCmdCompleteData->bInUse = IMG_FALSE;

	/* FIXME: This may cause unrelated devices to be woken up. */
	PVRSRVScheduleDeviceCallbacks();

	if(bScheduleMISR || OSInAtomic(psSysData))
	{
		OSScheduleMISR(psSysData);
	}

	if (bPinned)
	{
		CmdProcRefPut(ui32DevIndex);
	}
}

IMG_EXPORT
IMG_VOID PVRSRVCommandCompleteKM(IMG_HANDLE	hCmdCookie,
								 IMG_BOOL	bScheduleMISR)
{
	PVRSRVCommandCompleteStatusKM(hCmdCookie, bScheduleMISR, PVRSRV_COMMAND_STATUS_PRESENTED);
}


/*!
******************************************************************************

 @Function	PVRSRVRegisterCmdProcListKM

 @Description

 registers a list of private command processing functions with the Command
 Queue Manager

 @Input		ui32DevIndex : device index

 @Input		 ppfnCmdProcList : function ptr table of private command processors

 @Input		ui32MaxSyncsPerCmd : max number of syncobjects used by command

 @Input		ui32CmdCount : number of entries in function ptr table

 @Return	PVRSRV_ERROR

******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR PVRSRVRegisterCmdProcListKM(IMG_UINT32		ui32DevIndex,
										 PFN_CMD_PROC	*ppfnCmdProcList,
										 IMG_UINT32		ui32MaxSyncsPerCmd[][2],
										 IMG_UINT32		ui32CmdCount)
{
	SYS_DATA				*psSysData;
	PVRSRV_ERROR			eError;
	IMG_UINT32				ui32CmdCounter, ui32CmdTypeCounter;
	IMG_SIZE_T				ui32AllocSize;
	DEVICE_COMMAND_DATA		*psDeviceCommandData;
	COMMAND_COMPLETE_DATA	*psCmdCompleteData;

	/* validate device type */
	if(ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		PVR_DPF((PVR_DBG_ERROR,
					"PVRSRVRegisterCmdProcListKM: invalid DeviceType 0x%x",
					ui32DevIndex));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* acquire system data structure */
	SysAcquireData(&psSysData);

	/* array of pointers for each command store */
	ui32AllocSize = ui32CmdCount * sizeof(*psDeviceCommandData);
	eError = OSAllocMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
						ui32AllocSize,
						(IMG_VOID **)&psDeviceCommandData, IMG_NULL,
						"Array of Pointers for Command Store");
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVRegisterCmdProcListKM: Failed to alloc CC data"));
		goto ErrorExit;
	}

	psSysData->apsDeviceCommandData[ui32DevIndex] = psDeviceCommandData;
	OSMemSet(psDeviceCommandData->aulCompTimeHis, 0, DC_NUM_CMDS_COMPLETE_HISTORY*sizeof(unsigned long));
	psDeviceCommandData->ui32THWriteOffset = 0;

	/*
	 * Cycle 7/8: reset per-device accounting and publish RUNNING only after
	 * the table pointer above is visible, so an admission gate that observes
	 * RUNNING can always rely on the table pointer. On a later registration
	 * error the ErrorExit cleanup path freezes through the same state.
	 */
	atomic_set(&sCmdProcQueued[ui32DevIndex], 0);
	sFrozenCmdProc[ui32DevIndex] = IMG_NULL;
	smp_mb();
	atomic_set(&sCmdProcState[ui32DevIndex], CMD_PROC_STATE_RUNNING);

	for (ui32CmdTypeCounter = 0; ui32CmdTypeCounter < ui32CmdCount; ui32CmdTypeCounter++)
	{
		psDeviceCommandData[ui32CmdTypeCounter].pfnCmdProc = ppfnCmdProcList[ui32CmdTypeCounter];
		psDeviceCommandData[ui32CmdTypeCounter].ui32CCBOffset = 0;
		psDeviceCommandData[ui32CmdTypeCounter].ui32MaxDstSyncCount = ui32MaxSyncsPerCmd[ui32CmdTypeCounter][0];
		psDeviceCommandData[ui32CmdTypeCounter].ui32MaxSrcSyncCount = ui32MaxSyncsPerCmd[ui32CmdTypeCounter][1];
		for (ui32CmdCounter = 0; ui32CmdCounter < DC_NUM_COMMANDS_PER_TYPE; ui32CmdCounter++)
		{
			/*
				allocate storage for the sync update on command complete
			*/
			ui32AllocSize = sizeof(COMMAND_COMPLETE_DATA) /* space for one GENERIC_CMD_COMPLETE */
						  + ((ui32MaxSyncsPerCmd[ui32CmdTypeCounter][0]
						  +	ui32MaxSyncsPerCmd[ui32CmdTypeCounter][1])
						  * sizeof(PVRSRV_SYNC_OBJECT));	 /* space for max sync objects */

			eError = OSAllocMem(PVRSRV_OS_NON_PAGEABLE_HEAP,
								ui32AllocSize,
								(IMG_VOID **)&psCmdCompleteData,
								IMG_NULL,
								"Command Complete Data");
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR,"PVRSRVRegisterCmdProcListKM: Failed to alloc cmd %d", ui32CmdTypeCounter));
				goto ErrorExit;
			}
			
			psDeviceCommandData[ui32CmdTypeCounter].apsCmdCompleteData[ui32CmdCounter] = psCmdCompleteData;
			
			/* clear memory */
			OSMemSet(psCmdCompleteData, 0x00, ui32AllocSize);

			/* setup sync pointers */
			psCmdCompleteData->psDstSync = (PVRSRV_SYNC_OBJECT*)
											(((IMG_UINTPTR_T)psCmdCompleteData)
											+ sizeof(COMMAND_COMPLETE_DATA));
			psCmdCompleteData->psSrcSync = (PVRSRV_SYNC_OBJECT*)
											(((IMG_UINTPTR_T)psCmdCompleteData->psDstSync)
											+ (sizeof(PVRSRV_SYNC_OBJECT) * ui32MaxSyncsPerCmd[ui32CmdTypeCounter][0]));

			psCmdCompleteData->ui32AllocSize = (IMG_UINT32)ui32AllocSize;
		}
	}

	return PVRSRV_OK;

ErrorExit:

	/* clean-up if things went wrong */
	if (PVRSRVRemoveCmdProcListKM(ui32DevIndex, ui32CmdCount) != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVRegisterCmdProcListKM: Failed to clean up after error, device 0x%x",
				ui32DevIndex));
	}
	
	return eError;
}


/*!
******************************************************************************

 @Function	PVRSRVFreezeCmdProcKM

 @Description (Cycle 7) Reversible admission freeze. Transitions the device
 from RUNNING to CLOSING: no new display-swap admission can be granted, while
 the command-processor table stays published so already-queued commands keep
 dispatching and completing naturally through PVRSRVProcessQueues. Frozen
 state is not destructive and does not touch any command, sync counter or
 private data; PVRSRVUnfreezeCmdProcKM restores RUNNING exactly.

 Idempotent: freezing an already-frozen device succeeds. Freezing a detached
 device fails (nothing to freeze).

 @Input		ui32DevIndex : device index

 @Return	PVRSRV_ERROR (OK, INVALID_PARAMS)

 ******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR PVRSRVFreezeCmdProcKM(IMG_UINT32 ui32DevIndex)
{
	SYS_DATA		*psSysData;
	CMD_PROC_STATE	eState;

	if (ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVFreezeCmdProcKM: invalid DeviceType 0x%x",
				ui32DevIndex));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	SysAcquireData(&psSysData);

	for (;;)
	{
		eState = (CMD_PROC_STATE)atomic_read(&sCmdProcState[ui32DevIndex]);
		if (eState == CMD_PROC_STATE_CLOSING)
		{
			return PVRSRV_OK;	/* idempotent */
		}
		if (eState == CMD_PROC_STATE_DETACHED)
		{
			return PVRSRV_ERROR_INVALID_PARAMS;
		}
		if (atomic_cmpxchg(&sCmdProcState[ui32DevIndex],
				   CMD_PROC_STATE_RUNNING,
				   CMD_PROC_STATE_CLOSING) == CMD_PROC_STATE_RUNNING)
		{
			/* Capture the exact table this freeze covers; only it may
			 * be detached later. */
			sFrozenCmdProc[ui32DevIndex] =
				psSysData->apsDeviceCommandData[ui32DevIndex];
			smp_mb();
			return PVRSRV_OK;
		}
	}
}

/*!
******************************************************************************

 @Function	PVRSRVUnfreezeCmdProcKM

 @Description (Cycle 7) Reverses PVRSRVFreezeCmdProcKM: CLOSING -> RUNNING.
 Used only to roll back a failed DeInit preparation. Fails when the device is
 detached (a published table is a precondition of reopening admission).

 @Input		ui32DevIndex : device index

 @Return	PVRSRV_ERROR (OK, INVALID_PARAMS)

 ******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR PVRSRVUnfreezeCmdProcKM(IMG_UINT32 ui32DevIndex)
{
	SYS_DATA		*psSysData;
	CMD_PROC_STATE	eState;

	if (ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVUnfreezeCmdProcKM: invalid DeviceType 0x%x",
				ui32DevIndex));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	SysAcquireData(&psSysData);

	for (;;)
	{
		eState = (CMD_PROC_STATE)atomic_read(&sCmdProcState[ui32DevIndex]);
		if (eState == CMD_PROC_STATE_RUNNING)
		{
			return PVRSRV_OK;	/* idempotent */
		}
		if (eState == CMD_PROC_STATE_DETACHED)
		{
			return PVRSRV_ERROR_INVALID_PARAMS;
		}
		if (atomic_cmpxchg(&sCmdProcState[ui32DevIndex],
				   CMD_PROC_STATE_CLOSING,
				   CMD_PROC_STATE_RUNNING) == CMD_PROC_STATE_CLOSING)
		{
			sFrozenCmdProc[ui32DevIndex] = IMG_NULL;
			smp_mb();
			return PVRSRV_OK;
		}
	}
}

/*!
******************************************************************************

 @Function	PVRSRVDetachCmdProcKM

 @Description (Cycle 5/6/7) Unpublishes the command-processor table without
 freeing it. Preconditions (checked under the shared sQProcessResource lock):
 the device is CLOSING (admission frozen) and no command that still requires
 the table exists -- i.e. no entry is in use AND the submitted-command
 ownership count is zero. The ownership count is incremented in
 PVRSRVSubmitCommandKM (before the queue write offset publishes the command)
 and decremented only at the terminal pop in PVRSRVProcessQueues, which runs
 under this same lock, so a zero observation proves there is no
 submitted-but-undispatched or dependency-blocked command for the device.

 On success the table is parked in sDetachedCmdProc[] and the state becomes
 DETACHED; PVRSRVFreeDetachedCmdProcKM may then free it. On any failure the
 table stays published, the state stays CLOSING and nothing is modified
 (fail closed, retryable). Detach is never needed to roll back a failed
 DeInit: the only fallible lifecycle steps run before it.

 @Input		ui32DevIndex : device index

 @Input		ui32CmdCount : number of entries in function ptr table

 @Return	PVRSRV_ERROR (OK, RETRY, PROCESSING_BLOCKED, INVALID_PARAMS)

 ******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR PVRSRVDetachCmdProcKM(IMG_UINT32	ui32DevIndex,
								   IMG_UINT32	ui32CmdCount)
{
	SYS_DATA				*psSysData;
	IMG_UINT32				ui32CmdTypeCounter, ui32CmdCounter;
	DEVICE_COMMAND_DATA		*psDeviceCommandData;
	COMMAND_COMPLETE_DATA	*psCmdCompleteData;
	PVRSRV_ERROR			eError = PVRSRV_OK;

	/* validate device type */
	if(ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVDetachCmdProcKM: invalid DeviceType 0x%x",
				ui32DevIndex));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* acquire system data structure */
	SysAcquireData(&psSysData);

	if (OSLockResource(&psSysData->sQProcessResource, KERNEL_ID) != PVRSRV_OK)
	{
		/* Same contract as PVRSRVDestroyCommandQueueKM: retry later. */
		return PVRSRV_ERROR_RETRY;
	}

	if (atomic_read(&sCmdProcState[ui32DevIndex]) == CMD_PROC_STATE_DETACHED)
	{
		/* Already detached (parked or freed): idempotent success. */
		sFrozenCmdProc[ui32DevIndex] = IMG_NULL;
		goto UnlockExit;
	}

	if (atomic_read(&sCmdProcState[ui32DevIndex]) != CMD_PROC_STATE_CLOSING)
	{
		/* Not frozen: refusing to detach would strand dispatching work. */
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVDetachCmdProcKM: device not frozen (dev %u)",
				ui32DevIndex));
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto UnlockExit;
	}

	psDeviceCommandData = psSysData->apsDeviceCommandData[ui32DevIndex];
	if (psDeviceCommandData == IMG_NULL)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVDetachCmdProcKM: frozen without a published table (dev %u)",
				ui32DevIndex));
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto UnlockExit;
	}

	/*
	 * Cycle 8 device-ID reuse guard: detach may only act on the exact
	 * table captured by the freeze. If another registration (hypothetical
	 * in this tree - the DC lifecycle refuses Init while staged) replaced
	 * the table at this index, fail closed and do NOT touch, park,
	 * republish or free the foreign table.
	 */
	if (psDeviceCommandData != sFrozenCmdProc[ui32DevIndex])
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVDetachCmdProcKM: published table is not the frozen one (dev %u); refusing",
				ui32DevIndex));
		eError = PVRSRV_ERROR_PROCESSING_BLOCKED;
		goto UnlockExit;
	}

	/*
	 * Cycle 7 BLOCKER-1 invariant: a command that was submitted but not yet
	 * terminally popped keeps the table alive. Zero here (under the same
	 * lock that guards the popping loop) proves no such command exists.
	 */
	if (atomic_read(&sCmdProcQueued[ui32DevIndex]) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVDetachCmdProcKM: %d command(s) still queued for device %u",
				atomic_read(&sCmdProcQueued[ui32DevIndex]), ui32DevIndex));
		eError = PVRSRV_ERROR_PROCESSING_BLOCKED;
		goto UnlockExit;
	}

	/* No command completion may still be using a cookie from this table. */
	for (ui32CmdTypeCounter = 0; ui32CmdTypeCounter < ui32CmdCount; ui32CmdTypeCounter++)
	{
		for (ui32CmdCounter = 0; ui32CmdCounter < DC_NUM_COMMANDS_PER_TYPE; ui32CmdCounter++)
		{
			psCmdCompleteData = psDeviceCommandData[ui32CmdTypeCounter].apsCmdCompleteData[ui32CmdCounter];
			if (psCmdCompleteData != IMG_NULL && psCmdCompleteData->bInUse)
			{
				PVR_DPF((PVR_DBG_ERROR, "PVRSRVDetachCmdProcKM: command still in use (%u/%u)",
						ui32CmdTypeCounter, ui32CmdCounter));
				eError = PVRSRV_ERROR_PROCESSING_BLOCKED;
				goto UnlockExit;
			}
		}
	}

	PVR_ASSERT(sDetachedCmdProc[ui32DevIndex] == IMG_NULL);
	sDetachedCmdProc[ui32DevIndex] = psDeviceCommandData;
	psSysData->apsDeviceCommandData[ui32DevIndex] = IMG_NULL;
	sFrozenCmdProc[ui32DevIndex] = IMG_NULL;
	smp_mb();
	atomic_set(&sCmdProcState[ui32DevIndex], CMD_PROC_STATE_DETACHED);
	smp_mb();

UnlockExit:
	OSUnlockResource(&psSysData->sQProcessResource, KERNEL_ID);
	return eError;
}

/*!
******************************************************************************

 @Function	PVRSRVFreeDetachedCmdProcKM

 @Description (Cycle 5/7) Frees a table previously detached by
 PVRSRVDetachCmdProcKM. Safe to call when nothing is detached (no-op
 success), and retryable: any failure leaves the parked pointer intact for
 a later call. The drain wait runs without holding the lifecycle lock;
 freeing runs under it (same precedent as PVRSRVDestroyCommandQueueKM),
 after re-validating that no pin, no queued command and no in-use entry
 remain.

 @Input		ui32DevIndex : device index

 @Input		ui32CmdCount : number of entries in function ptr table

 @Return	PVRSRV_ERROR (OK, RETRY, PROCESSING_BLOCKED, INVALID_PARAMS)

 ******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR PVRSRVFreeDetachedCmdProcKM(IMG_UINT32	ui32DevIndex,
										 IMG_UINT32	ui32CmdCount)
{
	SYS_DATA				*psSysData;
	IMG_UINT32				ui32CmdTypeCounter, ui32CmdCounter;
	DEVICE_COMMAND_DATA		*psDeviceCommandData;
	COMMAND_COMPLETE_DATA	*psCmdCompleteData;
	IMG_SIZE_T				ui32AllocSize;
	PVRSRV_ERROR			eError = PVRSRV_OK;

	/* validate device type */
	if(ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		PVR_DPF((PVR_DBG_ERROR,
				"PVRSRVFreeDetachedCmdProcKM: invalid DeviceType 0x%x",
				ui32DevIndex));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	/* acquire system data structure */
	SysAcquireData(&psSysData);

	psDeviceCommandData = sDetachedCmdProc[ui32DevIndex];
	if (psDeviceCommandData == IMG_NULL)
	{
		return PVRSRV_OK;
	}

	/*
	 * Quiescence for pins taken just before the detach publication became
	 * visible (inc-then-load versus store-then-read pairing): such readers
	 * always observe NULL and bail without dereferencing, so this drains
	 * in moments. No lock is held; nothing here waits on the lifecycle
	 * lock, so this cannot deadlock. Timeout fails closed with the parked
	 * pointer intact.
	 */
	for (ui32CmdCounter = 0; ui32CmdCounter < CMDPROC_REF_DRAIN_MS; ui32CmdCounter++)
	{
		if (atomic_read(&sCmdProcRef[ui32DevIndex]) == 0)
		{
			break;
		}
		OSSleepms(1);
	}
	if (atomic_read(&sCmdProcRef[ui32DevIndex]) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVFreeDetachedCmdProcKM: readers did not drain"));
		return PVRSRV_ERROR_PROCESSING_BLOCKED;
	}

	if (OSLockResource(&psSysData->sQProcessResource, KERNEL_ID) != PVRSRV_OK)
	{
		return PVRSRV_ERROR_RETRY;
	}

	/*
	 * Re-validate under the lock: a leaked queue could have dispatched
	 * (setting bInUse) during the drain wait. Failure re-parks everything
	 * (table stays detached and parked) for a later retry.
	 */
	if (atomic_read(&sCmdProcQueued[ui32DevIndex]) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "PVRSRVFreeDetachedCmdProcKM: %d command(s) queued during drain",
				atomic_read(&sCmdProcQueued[ui32DevIndex])));
		eError = PVRSRV_ERROR_PROCESSING_BLOCKED;
		goto UnlockExit;
	}
	for (ui32CmdTypeCounter = 0; ui32CmdTypeCounter < ui32CmdCount; ui32CmdTypeCounter++)
	{
		for (ui32CmdCounter = 0; ui32CmdCounter < DC_NUM_COMMANDS_PER_TYPE; ui32CmdCounter++)
		{
			psCmdCompleteData = psDeviceCommandData[ui32CmdTypeCounter].apsCmdCompleteData[ui32CmdCounter];
			if (psCmdCompleteData != IMG_NULL && psCmdCompleteData->bInUse)
			{
				PVR_DPF((PVR_DBG_ERROR, "PVRSRVFreeDetachedCmdProcKM: command went in use during drain (%u/%u)",
						ui32CmdTypeCounter, ui32CmdCounter));
				eError = PVRSRV_ERROR_PROCESSING_BLOCKED;
				goto UnlockExit;
			}
		}
	}

	for (ui32CmdTypeCounter = 0; ui32CmdTypeCounter < ui32CmdCount; ui32CmdTypeCounter++)
	{
		for (ui32CmdCounter = 0; ui32CmdCounter < DC_NUM_COMMANDS_PER_TYPE; ui32CmdCounter++)
		{
			psCmdCompleteData = psDeviceCommandData[ui32CmdTypeCounter].apsCmdCompleteData[ui32CmdCounter];

			/* free the cmd complete structure array entries */
			if (psCmdCompleteData != IMG_NULL)
			{
				PVR_ASSERT(psCmdCompleteData->bInUse == IMG_FALSE);
				OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP, psCmdCompleteData->ui32AllocSize,
						  psCmdCompleteData, IMG_NULL);
				psDeviceCommandData[ui32CmdTypeCounter].apsCmdCompleteData[ui32CmdCounter] = IMG_NULL;
			}
		}
	}

	/* free the cmd complete structure array for the device */
	ui32AllocSize = ui32CmdCount * sizeof(*psDeviceCommandData);
	OSFreeMem(PVRSRV_OS_NON_PAGEABLE_HEAP, ui32AllocSize, psDeviceCommandData, IMG_NULL);
	sDetachedCmdProc[ui32DevIndex] = IMG_NULL;

UnlockExit:
	OSUnlockResource(&psSysData->sQProcessResource, KERNEL_ID);
	return eError;
}

/*!
******************************************************************************

 @Function	PVRSRVRemoveCmdProcListKM

 @Description

 removes a list of private command processing functions and data from the
 Queue Manager. Implemented as freeze (admission closed) + detach
 (submitted-command ownership must be zero) + free. Lock ownership is
 balanced on every path: this wrapper itself takes no lifecycle lock; the
 phases each balance theirs internally. All error returns leave retryable
 state. Also used by the register-error cleanup path, where the device may
 still be RUNNING.

 @Input		ui32DevIndex : device index

 @Input		ui32CmdCount : number of entries in function ptr table

 @Return	PVRSRV_ERROR

 ******************************************************************************/
IMG_EXPORT
PVRSRV_ERROR PVRSRVRemoveCmdProcListKM(IMG_UINT32 ui32DevIndex,
									   IMG_UINT32 ui32CmdCount)
{
	PVRSRV_ERROR eError;

	/* Idempotent when already frozen; fails only when already detached. */
	(void)PVRSRVFreezeCmdProcKM(ui32DevIndex);

	eError = PVRSRVDetachCmdProcKM(ui32DevIndex, ui32CmdCount);
	if (eError != PVRSRV_OK)
	{
		return eError;
	}

	return PVRSRVFreeDetachedCmdProcKM(ui32DevIndex, ui32CmdCount);
}

/*!
******************************************************************************
 @Function	PVRSRVAcquireCmdProcRefKM / PVRSRVReleaseCmdProcRefKM

 @Description (Cycle 5/7) Opaque admission pins for display-swap insertion
 paths, see queue.h. Acquire succeeds only while the device is RUNNING;
 a NULL return takes no reference and must not be released. Bare atomics
 only.

 ******************************************************************************
*/
IMG_EXPORT
IMG_HANDLE IMG_CALLCONV PVRSRVAcquireCmdProcRefKM(IMG_UINT32 ui32DevIndex)
{
	SYS_DATA *psSysData;

	if (ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		return IMG_NULL;
	}
	SysAcquireData(&psSysData);
	return (IMG_HANDLE)CmdProcAdmitGet(psSysData, ui32DevIndex);
}

IMG_EXPORT
IMG_VOID IMG_CALLCONV PVRSRVReleaseCmdProcRefKM(IMG_UINT32 ui32DevIndex)
{
	CmdProcRefPut(ui32DevIndex);
}

/*!
******************************************************************************
 @Function	PVRSRVQueryCmdProcIdleKM

 @Description (Cycle 4/5/7) Non-mutating probe: reports whether the device
 has no outstanding command-processor work. Idle requires all of:
   - zero reader/admission pins (an insertion transaction in progress
     counts as busy even with no entry in use yet);
   - zero submitted-but-not-popped commands (a queued command awaiting
     dispatch, a dependency-blocked command, or a command whose pop has
     not completed yet, all count as busy);
   - no command-complete entry in use.
 Used by display DeInit preparation. Never detaches or frees. Returns
 IMG_FALSE when busy or when the lifecycle lock cannot be taken (fail
 closed).

 @Input		ui32DevIndex : device index
 @Input		ui32CmdCount : number of entries in function ptr table

 @Return	IMG_BOOL
******************************************************************************
*/
IMG_EXPORT
IMG_BOOL IMG_CALLCONV PVRSRVQueryCmdProcIdleKM(IMG_UINT32	ui32DevIndex,
											   IMG_UINT32	ui32CmdCount)
{
	SYS_DATA				*psSysData;
	IMG_UINT32				ui32CmdTypeCounter, ui32CmdCounter;
	DEVICE_COMMAND_DATA		*psDeviceCommandData;
	COMMAND_COMPLETE_DATA	*psCmdCompleteData;
	IMG_BOOL				bIdle = IMG_FALSE;

	/* validate device type */
	if(ui32DevIndex >= SYS_DEVICE_COUNT)
	{
		return IMG_FALSE;
	}

	/* acquire system data structure */
	SysAcquireData(&psSysData);

	/* Non-blocking lifecycle lock: failure means busy (fail closed). */
	if (OSLockResource(&psSysData->sQProcessResource, KERNEL_ID) != PVRSRV_OK)
	{
		return IMG_FALSE;
	}

	psDeviceCommandData = psSysData->apsDeviceCommandData[ui32DevIndex];
	if (psDeviceCommandData != IMG_NULL)
	{
		bIdle = IMG_TRUE;
		if (atomic_read(&sCmdProcRef[ui32DevIndex]) != 0)
		{
			bIdle = IMG_FALSE;
		}
		if (atomic_read(&sCmdProcQueued[ui32DevIndex]) != 0)
		{
			bIdle = IMG_FALSE;
		}
		for (ui32CmdTypeCounter = 0;
		     ui32CmdTypeCounter < ui32CmdCount && bIdle;
		     ui32CmdTypeCounter++)
		{
			for (ui32CmdCounter = 0;
			     ui32CmdCounter < DC_NUM_COMMANDS_PER_TYPE;
			     ui32CmdCounter++)
			{
				psCmdCompleteData = psDeviceCommandData[ui32CmdTypeCounter].apsCmdCompleteData[ui32CmdCounter];
				if (psCmdCompleteData != IMG_NULL && psCmdCompleteData->bInUse)
				{
					bIdle = IMG_FALSE;
					break;
				}
			}
		}
	}
	else
	{
		/* Detached: no command can be dispatched through it. Queued
		 * ownership must already be zero (detach requires it); report
		 * busy if not so the caller cannot mistake inconsistent state
		 * for quiescence. */
		bIdle = (atomic_read(&sCmdProcQueued[ui32DevIndex]) == 0);
	}

	OSUnlockResource(&psSysData->sQProcessResource, KERNEL_ID);

	return bIdle;
}

/******************************************************************************
 End of file (queue.c)
******************************************************************************/
