/*************************************************************************/ /*!
@Title          Command Queue API
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Internal structures and definitions for command queues
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

#ifndef QUEUE_H
#define QUEUE_H

#if defined(SUPPORT_PVRSRV_DEVICE_CLASS)

#if defined(__cplusplus)
extern "C" {
#endif

/*!
 * Macro to Read Offset in given command queue
 */
#define UPDATE_QUEUE_ROFF(psQueue, uSize)						\
	(psQueue)->uReadOffset = ((psQueue)->uReadOffset + (uSize))	\
	& ((psQueue)->uQueueSize - 1);

#ifndef _PVRSRV_COMMAND_STATUS_DEFINED_
#define _PVRSRV_COMMAND_STATUS_DEFINED_
/*
 * Cycle 4 architectural note: this status is recorded in
 * COMMAND_COMPLETE_DATA.eStatus by PVRSRVCommandCompleteStatusKM, but the
 * Services 1.x display queue has no failure-reporting channel to
 * fences or userspace. Sync accounting (WriteOps/ReadOps2Complete),
 * command-private-data release and queue pop run identically for every
 * status so the queue can never wedge on a failed frame; the sw_sync
 * timeline is only advanced for PRESENTED (and is a no-op stub in this
 * build, with pre-signalled stub fences). A FAILED frame is therefore
 * observable only via the driver failure counter/rate-limited log, and
 * the display keeps showing the previous frame. Do not map FAILED to
 * PRESENTED: the stored status must stay honest.
 */
typedef enum _PVRSRV_COMMAND_STATUS_
{
	PVRSRV_COMMAND_STATUS_PRESENTED = 0, /* Frame successfully presented to display */
	PVRSRV_COMMAND_STATUS_FAILED    = 1, /* Terminal failure: not presented */
	PVRSRV_COMMAND_STATUS_ABORTED   = 2, /* Aborted during queue/device teardown */
} PVRSRV_COMMAND_STATUS;
#endif

/*!
	generic cmd complete structure.
	This structure represents the storage required between starting and finishing
	a given cmd and is required to hold the generic sync object update data.
	note: for any given system we know what command types we support and
	therefore how much storage is required for any number of commands in progress
 */
 typedef struct _COMMAND_COMPLETE_DATA_
 {
	IMG_BOOL			bInUse;
	PVRSRV_COMMAND_STATUS eStatus;		/*!< Presentation/terminal completion status */
	/* <arg(s) to PVRSRVProcessQueues>;	*/	/*!< TBD */
	IMG_UINT32			ui32DstSyncCount;	/*!< number of dst sync objects */
	IMG_UINT32			ui32SrcSyncCount;	/*!< number of src sync objects */
	PVRSRV_SYNC_OBJECT	*psDstSync;			/*!< dst sync ptr list, 
                                        	allocated on back of this structure */
	PVRSRV_SYNC_OBJECT	*psSrcSync;			/*!< src sync ptr list, 
                                       		allocated on back of this structure */
	IMG_UINT32			ui32AllocSize;		/*!< allocated size*/
	PFN_QUEUE_COMMAND_COMPLETE	pfnCommandComplete;	/*!< Command complete callback */
	IMG_HANDLE					hCallbackData;		/*!< Command complete callback data */
	IMG_UINT32					ui32Stamp;
	unsigned long               ulQueueTime;
	IMG_UINT32			ui32DevIndex;

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	IMG_VOID			*pvCleanupFence;	/*!< Sync fence to 'put' after timeline inc() */
	IMG_VOID			*pvTimeline;		/*!< Android sync timeline to inc() */
#endif
 }COMMAND_COMPLETE_DATA, *PCOMMAND_COMPLETE_DATA;

#if !defined(USE_CODE)
IMG_VOID QueueDumpDebugInfo(IMG_VOID);

IMG_IMPORT
PVRSRV_ERROR PVRSRVProcessQueues (IMG_BOOL		bFlush);

#if defined(__linux__) && defined(__KERNEL__) 
#include <linux/types.h>
#include <linux/seq_file.h>
void* ProcSeqOff2ElementQueue(struct seq_file * sfile, loff_t off);
void ProcSeqShowQueue(struct seq_file *sfile,void* el);
#endif


IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVCreateCommandQueueKM(IMG_SIZE_T uQueueSize,
													 PVRSRV_QUEUE_INFO **ppsQueueInfo);
IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVDestroyCommandQueueKM(PVRSRV_QUEUE_INFO *psQueueInfo);

IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVInsertCommandKM(PVRSRV_QUEUE_INFO	*psQueue,
												PVRSRV_COMMAND		**ppsCommand,
												IMG_UINT32			ui32DevIndex,
												IMG_UINT16			CommandType,
												IMG_UINT32			ui32DstSyncCount,
												PVRSRV_KERNEL_SYNC_INFO	*apsDstSync[],
												IMG_UINT32			ui32SrcSyncCount,
												PVRSRV_KERNEL_SYNC_INFO	*apsSrcSync[],
												IMG_SIZE_T			ui32DataByteSize,
												PFN_QUEUE_COMMAND_COMPLETE pfnCommandComplete,
												IMG_HANDLE			hCallbackData,
												IMG_HANDLE			*phFence);

IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVGetQueueSpaceKM(PVRSRV_QUEUE_INFO *psQueue,
												IMG_SIZE_T uParamSize,
												IMG_VOID **ppvSpace);

IMG_IMPORT
PVRSRV_ERROR IMG_CALLCONV PVRSRVSubmitCommandKM(PVRSRV_QUEUE_INFO *psQueue,
												PVRSRV_COMMAND *psCommand);

IMG_IMPORT
IMG_VOID PVRSRVCommandCompleteStatusKM(IMG_HANDLE hCmdCookie, IMG_BOOL bScheduleMISR, PVRSRV_COMMAND_STATUS eStatus);

IMG_IMPORT
IMG_VOID PVRSRVCommandCompleteKM(IMG_HANDLE hCmdCookie, IMG_BOOL bScheduleMISR);

IMG_IMPORT
PVRSRV_ERROR PVRSRVRegisterCmdProcListKM(IMG_UINT32		ui32DevIndex,
										 PFN_CMD_PROC	*ppfnCmdProcList,
										 IMG_UINT32		ui32MaxSyncsPerCmd[][2],
										 IMG_UINT32		ui32CmdCount);
/*
 * Cycle 4/5/7 lifetime contract. The command-processor table has an explicit
 * per-device lifecycle:
 *
 *   DETACHED (initial; no table)
 *       --[PVRSRVRegisterCmdProcListKM publishes table]--> RUNNING
 *   RUNNING (admission open)
 *       --[PVRSRVFreezeCmdProcKM]--> CLOSING (admission rejected; the table
 *                                    stays published and already-queued
 *                                    commands keep dispatching/completing
 *                                    naturally)
 *   CLOSING --[PVRSRVDetachCmdProcKM: requires every command submitted for
 *              this device to have been terminally popped (queued count 0)
 *              and no entry in use, checked under sQProcessResource]-->
 *           DETACHED (table unpublished + parked; no insertion can be
 *                     admitted and no dispatch can start)
 *       --[PVRSRVFreeDetachedCmdProcKM: drains pins under the same
 *          lock discipline]--> table freed
 *   CLOSING --[PVRSRVUnfreezeCmdProcKM]--> RUNNING (reversible freeze)
 *
 * Removal is serialised against queue processing with the shared
 * sQProcessResource lifecycle lock (check + detach are atomic w.r.t.
 * PVRSRVProcessQueues, under which every ProcessFlip runs); freeing waits for
 * non-queue-lock readers (completions, inserts, admission pins, dump) tracked
 * by an atomic reader count. Must not be called while holding any display-side
 * operation mutex (lock ordering: queue lifecycle lock is a leaf).
 *
 * Cycle 7 ownership: a submitted command keeps command-processor ownership
 * from PVRSRVSubmitCommandKM (before the queue write offset is published)
 * until PVRSRVProcessQueues terminally pops it (after UPDATE_QUEUE_ROFF), so
 * detach cannot strand a submitted-but-undispatched command. Detach refuses
 * with PVRSRV_ERROR_PROCESSING_BLOCKED while such commands exist.
 */
IMG_IMPORT
PVRSRV_ERROR PVRSRVFreezeCmdProcKM(IMG_UINT32	ui32DevIndex);
IMG_IMPORT
PVRSRV_ERROR PVRSRVUnfreezeCmdProcKM(IMG_UINT32	ui32DevIndex);
IMG_IMPORT
PVRSRV_ERROR PVRSRVDetachCmdProcKM(IMG_UINT32	ui32DevIndex,
								   IMG_UINT32	ui32CmdCount);
IMG_IMPORT
PVRSRV_ERROR PVRSRVFreeDetachedCmdProcKM(IMG_UINT32	ui32DevIndex,
										 IMG_UINT32	ui32CmdCount);
IMG_IMPORT
PVRSRV_ERROR PVRSRVRemoveCmdProcListKM(IMG_UINT32	ui32DevIndex,
									   IMG_UINT32	ui32CmdCount);

/*
 * Cycle 4/5/7: non-mutating probe used by display DeInit preparation.
 * Idle requires zero reader references AND zero submitted-but-undispatched
 * commands AND no command entry in use, so an insertion transaction, a
 * queued command awaiting dispatch, a dependency-blocked command, or any
 * other pin all count as busy even though dispatch has not run yet.
 * Returns IMG_TRUE when quiesced. Returns IMG_FALSE when busy or when the
 * lifecycle lock cannot be taken (fail closed). Never detaches or frees.
 */
IMG_IMPORT
IMG_BOOL IMG_CALLCONV PVRSRVQueryCmdProcIdleKM(IMG_UINT32	ui32DevIndex,
											   IMG_UINT32	ui32CmdCount);

/*
 * Cycle 5/7: opaque admission pins for display-swap insertion paths
 * (deviceclass.c). Acquire succeeds only while the device is RUNNING, pins
 * the command-processor table and returns it as an opaque handle
 * (IMG_NULL = frozen/detached: fail closed without dereferencing and with
 * no reference taken); Release drops the pin. The pin must be held across
 * the whole validation/insert/fill/submit transaction so freeze/detach
 * cannot commit mid-admission, and released on every exit (including
 * unwinds). Bare atomics only: safe in any context, never blocks, never
 * takes a lock. A leaked pin only delays teardown fail-closed; it cannot
 * corrupt.
 */
IMG_IMPORT IMG_HANDLE IMG_CALLCONV PVRSRVAcquireCmdProcRefKM(IMG_UINT32 ui32DevIndex);
IMG_IMPORT IMG_VOID IMG_CALLCONV PVRSRVReleaseCmdProcRefKM(IMG_UINT32 ui32DevIndex);

#endif /* !defined(USE_CODE) */


#if defined (__cplusplus)
}
#endif

#endif /* defined(SUPPORT_PVRSRV_DEVICE_CLASS) */

#endif /* QUEUE_H */

/******************************************************************************
 End of file (queue.h)
******************************************************************************/
