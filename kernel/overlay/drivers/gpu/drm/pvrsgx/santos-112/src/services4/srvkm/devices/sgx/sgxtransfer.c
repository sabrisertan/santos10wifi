/*************************************************************************/ /*!
@Title          Device specific transfer queue routines
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

#if defined(TRANSFER_QUEUE)

#include <stddef.h>

#include "sgxdefs.h"
#include "services_headers.h"
#include "buffer_manager.h"
#include "sgxinfo.h"
#include "sysconfig.h"
#include "pdump_km.h"
#include "mmu.h"
#include "pvr_bridge.h"
#include "sgx_bridge_km.h"
#include "sgxinfokm.h"
#include "osfunc.h"
#include "pvr_debug.h"
#include "sgxutils.h"
#include "ttrace.h"

/*
 * Shared pre-submit unwind for every sync operation taken for a transfer
 * that was not handed to SGXScheduleCCBCommandKM():
 *
 *   - source read ops and destination write ops, only inside the nominal
 *     counts and only for enabled entries (a disabled duplicate was never
 *     taken); only when the command was not KEEPPENDING;
 *   - the TA and 3D write ops (taken unconditionally while the shared
 *     command was built);
 *   - the M3 provider syncinfo write op exactly once: if it is one of the
 *     destination entries it is skipped in the loop above and undone here,
 *     otherwise (prepared but not yet slotted) it is undone here directly.
 *
 * The direction is always the inverse of the corresponding SyncTake*Op():
 * SyncRollBackWriteOp() for TA/3D/dst/M3 and SyncRollBackReadOp() for src.
 * PDUMP shadow counters are decremented only when bPdumpTaken is true (the
 * PDUMP update section ran after the command was built); early failures
 * (claim/selection/prepare) must not touch them.
 */
static IMG_VOID SGXTransferUnwindSyncOps(PVRSRV_TRANSFER_SGX_KICK *psKick,
					 IMG_BOOL abSrcSyncEnable[],
					 IMG_BOOL abDstSyncEnable[],
					 PVRSRV_KERNEL_SYNC_INFO *psFenceSyncInfo,
					 IMG_BOOL bPdumpTaken)
{
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
	IMG_UINT32 loop;

	if ((psKick->ui32Flags & SGXMKIF_TQFLAGS_KEEPPENDING) == 0UL)
	{
		for (loop = 0; loop < psKick->ui32NumDstSync; loop++)
		{
			if (!abDstSyncEnable[loop])
				continue;

			psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahDstSyncInfo[loop];
			if (psFenceSyncInfo && psFenceSyncInfo == psSyncInfo)
				continue;	/* M3 object is undone once, below */

			SyncRollBackWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);
#if defined(PDUMP)
			if (bPdumpTaken &&
			    (PDumpIsCaptureFrameKM() ||
			     ((psKick->ui32PDumpFlags & PDUMP_FLAGS_CONTINUOUS) != 0)))
			{
				psSyncInfo->psSyncData->ui32LastOpDumpVal--;
			}
#endif
		}

		for (loop = 0; loop < psKick->ui32NumSrcSync; loop++)
		{
			if (!abSrcSyncEnable[loop])
				continue;

			psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[loop];
			SyncRollBackReadOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);
#if defined(PDUMP)
			if (bPdumpTaken &&
			    (PDumpIsCaptureFrameKM() ||
			     ((psKick->ui32PDumpFlags & PDUMP_FLAGS_CONTINUOUS) != 0)))
			{
				psSyncInfo->psSyncData->ui32LastReadOpDumpVal--;
			}
#endif
		}
	}

	if (psKick->hTASyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->hTASyncInfo;
		SyncRollBackWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);
#if defined(PDUMP)
		if (bPdumpTaken &&
		    (PDumpIsCaptureFrameKM() ||
		     ((psKick->ui32PDumpFlags & PDUMP_FLAGS_CONTINUOUS) != 0)))
		{
			psSyncInfo->psSyncData->ui32LastOpDumpVal--;
		}
#endif
	}

	if (psKick->h3DSyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->h3DSyncInfo;
		SyncRollBackWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);
#if defined(PDUMP)
		if (bPdumpTaken &&
		    (PDumpIsCaptureFrameKM() ||
		     ((psKick->ui32PDumpFlags & PDUMP_FLAGS_CONTINUOUS) != 0)))
		{
			psSyncInfo->psSyncData->ui32LastOpDumpVal--;
		}
#endif
	}

	if (psFenceSyncInfo)
	{
		SyncRollBackWriteOp(psFenceSyncInfo, SYNC_OP_CLASS_TQ_3D);
#if defined(PDUMP)
		if (bPdumpTaken &&
		    (PDumpIsCaptureFrameKM() ||
		     ((psKick->ui32PDumpFlags & PDUMP_FLAGS_CONTINUOUS) != 0)))
		{
			psFenceSyncInfo->psSyncData->ui32LastOpDumpVal--;
		}
#endif
	}
}


#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
#include "pvr_sync.h"

/* Map the provider bind errors to the services error space. */
static PVRSRV_ERROR SGXTransferBindError(int iRet)
{
	if (iRet == -EBUSY)
		return PVRSRV_ERROR_RETRY;		/* another submit owns the alloc */
	if (iRet == -EALREADY || iRet == -EPERM)
		return PVRSRV_ERROR_HANDLE_NOT_FOUND;	/* consumed / not an alloc fd */
	return PVRSRV_ERROR_OUT_OF_MEMORY;
}
#endif /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC) */

IMG_EXPORT PVRSRV_ERROR SGXSubmitTransferKM(IMG_HANDLE hDevHandle, PVRSRV_TRANSFER_SGX_KICK *psKick)
{
	PVRSRV_KERNEL_MEM_INFO		*psCCBMemInfo = (PVRSRV_KERNEL_MEM_INFO *)psKick->hCCBMemInfo;
	SGXMKIF_COMMAND				sCommand = {0};
	SGXMKIF_TRANSFERCMD_SHARED *psSharedTransferCmd;
	PVRSRV_KERNEL_SYNC_INFO 	*psSyncInfo;
	PVRSRV_ERROR				eError;
	IMG_UINT32					loop;
	IMG_HANDLE					hDevMemContext = IMG_NULL;
	PVRSRV_KERNEL_SYNC_INFO		*psFenceSyncInfo = IMG_NULL;
	IMG_BOOL					abSrcSyncEnable[SGX_MAX_TRANSFER_SYNC_OPS];
	IMG_UINT32					ui32RealSrcSyncNum = 0;
	IMG_BOOL					abDstSyncEnable[SGX_MAX_TRANSFER_SYNC_OPS];
	IMG_UINT32					ui32RealDstSyncNum = 0;
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	/* L11 M3: real SGX completion object bound to the ALLOC_FENCE fd. */
	struct santos_sync_transfer_bind sFenceBind;
#endif


#if defined(PDUMP)
	IMG_BOOL bPersistentProcess = IMG_FALSE;
	/*
	 *	For persistent processes, the HW kicks should not go into the
	 *	extended init phase; only keep memory transactions from the
	 *	window system which are necessary to run the client app.
	 */
	{
		PVRSRV_PER_PROCESS_DATA* psPerProc = PVRSRVFindPerProcessData();
		if(psPerProc != IMG_NULL)
		{
			bPersistentProcess = psPerProc->bPDumpPersistent;
		}
	}
#endif /* PDUMP */
#if defined(FIX_HW_BRN_31620)
	hDevMemContext = psKick->hDevMemContext;
#endif
	PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_ENTER, TRANSFER_TOKEN_SUBMIT);

	for (loop = 0; loop < SGX_MAX_TRANSFER_SYNC_OPS; loop++)
	{
		abSrcSyncEnable[loop] = IMG_TRUE;
		abDstSyncEnable[loop] = IMG_TRUE;
	}

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	PVRSyncProviderBindInit(&sFenceBind);
#endif

	if (!CCB_OFFSET_IS_VALID(SGXMKIF_TRANSFERCMD_SHARED, psCCBMemInfo, psKick, ui32SharedCmdCCBOffset))
	{
		PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: Invalid CCB offset"));
		PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
				TRANSFER_TOKEN_SUBMIT);
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	/* override QAC warning about stricter alignment */
	/* PRQA S 3305 1 */
	psSharedTransferCmd =  CCB_DATA_FROM_OFFSET(SGXMKIF_TRANSFERCMD_SHARED, psCCBMemInfo, psKick, ui32SharedCmdCCBOffset);

	PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_CMD_START, TRANSFER_TOKEN_SUBMIT);
	PVR_TTRACE_UI32(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_CCB,
			TRANSFER_TOKEN_CCB_OFFSET, psKick->ui32SharedCmdCCBOffset);

	if (psKick->hTASyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->hTASyncInfo;

		PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_TRANSFER, TRANSFER_TOKEN_TA_SYNC,
					  psSyncInfo, PVRSRV_SYNCOP_SAMPLE);

		psSharedTransferCmd->ui32TASyncWriteOpsPendingVal = SyncTakeWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);
		psSharedTransferCmd->ui32TASyncReadOpsPendingVal  = psSyncInfo->psSyncData->ui32ReadOpsPending;

		psSharedTransferCmd->sTASyncWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
		psSharedTransferCmd->sTASyncReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
	}
	else
	{
		psSharedTransferCmd->sTASyncWriteOpsCompleteDevVAddr.uiAddr = 0;
		psSharedTransferCmd->sTASyncReadOpsCompleteDevVAddr.uiAddr = 0;
	}

	if (psKick->h3DSyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->h3DSyncInfo;

		PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_TRANSFER, TRANSFER_TOKEN_3D_SYNC,
					  psSyncInfo, PVRSRV_SYNCOP_SAMPLE);

		psSharedTransferCmd->ui323DSyncWriteOpsPendingVal = SyncTakeWriteOp(psSyncInfo,SYNC_OP_CLASS_TQ_3D);
		psSharedTransferCmd->ui323DSyncReadOpsPendingVal = psSyncInfo->psSyncData->ui32ReadOpsPending;

		psSharedTransferCmd->s3DSyncWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
		psSharedTransferCmd->s3DSyncReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
	}
	else
	{
		psSharedTransferCmd->s3DSyncWriteOpsCompleteDevVAddr.uiAddr = 0;
		psSharedTransferCmd->s3DSyncReadOpsCompleteDevVAddr.uiAddr = 0;
	}

	/* filter out multiple occurrences of the same sync object from srcs or dests
	 * note : the same sync can still be used to synchronize both src and dst.
	 */
	for (loop = 0; loop < MIN(SGX_MAX_TRANSFER_SYNC_OPS, psKick->ui32NumSrcSync); loop++)
	{
		IMG_UINT32 i;

		PVRSRV_KERNEL_SYNC_INFO * psMySyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[loop];
	
		for (i = 0; i < loop; i++)	
		{
			if (abSrcSyncEnable[i])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[i];

				if (psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr == psMySyncInfo->sWriteOpsCompleteDevVAddr.uiAddr)
				{
					PVR_DPF((PVR_DBG_WARNING, "SGXSubmitTransferKM : Same src synchronized multiple times!"));
					abSrcSyncEnable[loop] = IMG_FALSE;
					break;
				}
			}
		}
		if (abSrcSyncEnable[loop])
		{
			ui32RealSrcSyncNum++;
		}
	}
	for (loop = 0; loop < MIN(SGX_MAX_TRANSFER_SYNC_OPS, psKick->ui32NumDstSync); loop++)
	{
		IMG_UINT32 i;

		PVRSRV_KERNEL_SYNC_INFO * psMySyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahDstSyncInfo[loop];
	
		for (i = 0; i < loop; i++)	
		{
			if (abDstSyncEnable[i])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahDstSyncInfo[i];

				if (psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr == psMySyncInfo->sWriteOpsCompleteDevVAddr.uiAddr)
				{
					PVR_DPF((PVR_DBG_WARNING, "SGXSubmitTransferKM : Same dst synchronized multiple times!"));
					abDstSyncEnable[loop] = IMG_FALSE;
					break;
				}
			}
		}
		if (abDstSyncEnable[loop])
		{
			ui32RealDstSyncNum++;
		}
	}

	if ((psKick->ui32Flags & SGXMKIF_TQFLAGS_KEEPPENDING) == 0UL)
	{
		IMG_UINT32 i = 0;

		for (loop = 0; loop < psKick->ui32NumSrcSync; loop++)
		{
			if (abSrcSyncEnable[loop])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[loop];

				PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_TRANSFER, TRANSFER_TOKEN_SRC_SYNC,
						psSyncInfo, PVRSRV_SYNCOP_SAMPLE);

				psSharedTransferCmd->asSrcSyncs[i].ui32WriteOpsPendingVal = psSyncInfo->psSyncData->ui32WriteOpsPending;
				psSharedTransferCmd->asSrcSyncs[i].ui32ReadOpsPendingVal = psSyncInfo->psSyncData->ui32ReadOpsPending;

				psSharedTransferCmd->asSrcSyncs[i].sWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr; 
				psSharedTransferCmd->asSrcSyncs[i].sReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
				i++;
			}
		}
		PVR_ASSERT(i == ui32RealSrcSyncNum);

		i = 0;
		for (loop = 0; loop < psKick->ui32NumDstSync; loop++)
		{
			if (abDstSyncEnable[loop])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahDstSyncInfo[loop];

				psSyncInfo->psSyncData->ui64LastWrite = ui64KickCount;

				PVR_TTRACE_SYNC_OBJECT(PVRSRV_TRACE_GROUP_TRANSFER, TRANSFER_TOKEN_DST_SYNC,
						psSyncInfo, PVRSRV_SYNCOP_SAMPLE);

				psSharedTransferCmd->asDstSyncs[i].ui32WriteOpsPendingVal = psSyncInfo->psSyncData->ui32WriteOpsPending;
				psSharedTransferCmd->asDstSyncs[i].ui32ReadOpsPendingVal = psSyncInfo->psSyncData->ui32ReadOpsPending;
				psSharedTransferCmd->asDstSyncs[i].ui32ReadOps2PendingVal = psSyncInfo->psSyncData->ui32ReadOps2Pending;

				psSharedTransferCmd->asDstSyncs[i].sWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
				psSharedTransferCmd->asDstSyncs[i].sReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
				psSharedTransferCmd->asDstSyncs[i].sReadOps2CompleteDevVAddr = psSyncInfo->sReadOps2CompleteDevVAddr;
				i++;
			}
		}
		PVR_ASSERT(i == ui32RealDstSyncNum);

		/*
		 * We allow source and destination sync objects to be the
		 * same, which is why the read/write pending updates are delayed
		 * until the transfer command has been updated with the current
		 * values from the objects.
		 */
		for (loop = 0; loop < psKick->ui32NumSrcSync; loop++)
		{
			if (abSrcSyncEnable[loop])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[loop];
				SyncTakeReadOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);
			}
		}
		for (loop = 0; loop < psKick->ui32NumDstSync; loop++)
		{
			if (abDstSyncEnable[loop])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahDstSyncInfo[loop];
				SyncTakeWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);
			}
		}

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
		if (psKick->iFenceFd > 0)
		{
			int iRet;

			/*
			 * L11 M3: identify and reserve the core alloc *before* the
			 * transfer is allowed to submit.  The claim performs the
			 * exact alloc-fops validation and atomically rejects a
			 * fence fd, a consumed alloc or an alloc already owned by
			 * another in-flight submit.
			 */
			iRet = PVRSyncProviderBindClaim((int)(psKick->iFenceFd - 1), &sFenceBind);
			if (iRet == -EINVAL)
			{
				/*
				 * Not a core alloc fd (legacy shell stub) or no
				 * resolverable fd: the accepted stub contract adds no
				 * dependency and the kick proceeds.
				 */
			}
			else if (iRet != 0)
			{
				SGXTransferUnwindSyncOps(psKick, abSrcSyncEnable, abDstSyncEnable, IMG_NULL, IMG_FALSE);

				PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: M3 alloc claim failed (%d)", iRet));
				PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
						   TRANSFER_TOKEN_SUBMIT);
				return SGXTransferBindError(iRet);
			}
			else
			{
				IMG_UINT32 ui32FenceSlot;
				IMG_BOOL bAppend;

				/*
				 * Choose the destination slot.  A disabled entry is a
				 * duplicate that was filtered out of the packed CCB
				 * command; only a *trailing* run of such entries can be
				 * reused (it maps to the same compacted position as an
				 * append and keeps the nominal bookkeeping order equal
				 * to the packed order).  Otherwise the sync is appended
				 * after the nominal entries.  An enabled original
				 * destination is never overwritten.
				 */
				for (ui32FenceSlot = psKick->ui32NumDstSync;
				     ui32FenceSlot > 0;
				     ui32FenceSlot--)
				{
					if (abDstSyncEnable[ui32FenceSlot - 1])
						break;
				}
				bAppend = (ui32FenceSlot == psKick->ui32NumDstSync);

				/*
				 * Capacity must hold for both the nominal array and the
				 * real (duplicate-filtered) count.  A claimed core alloc
				 * must never be submitted without its completion sync,
				 * so this is a hard pre-submit failure: drop the claim
				 * and unwind every taken sync operation.
				 */
				if (ui32FenceSlot + 1 > SGX_MAX_TRANSFER_SYNC_OPS ||
				    ui32RealDstSyncNum + 1 > SGX_MAX_TRANSFER_SYNC_OPS)
				{
					PVRSyncProviderBindAbort(&sFenceBind);
					SGXTransferUnwindSyncOps(psKick, abSrcSyncEnable, abDstSyncEnable, IMG_NULL, IMG_FALSE);

					PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: no destination sync slot "
								"for the claimed alloc (nominal %u real %u)",
								psKick->ui32NumDstSync, ui32RealDstSyncNum));
					PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
							   TRANSFER_TOKEN_SUBMIT);
					return PVRSRV_ERROR_INVALID_PARAMS;
				}

				/*
				 * Prepare the real completion object.  Every fallible
				 * step happens before the provider write op is taken,
				 * and a failure drops the claim itself; the caller only
				 * has to unwind the native transfer ops.
				 */
				iRet = PVRSyncProviderBindPrepare(&sFenceBind,
								  &psSharedTransferCmd->asDstSyncs[ui32RealDstSyncNum]);
				if (iRet != 0)
				{
					SGXTransferUnwindSyncOps(psKick, abSrcSyncEnable, abDstSyncEnable, IMG_NULL, IMG_FALSE);

					PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: M3 prepare failed (%d)", iRet));
					PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
							   TRANSFER_TOKEN_SUBMIT);
					return SGXTransferBindError(iRet);
				}

				psFenceSyncInfo = sFenceBind.psSyncInfo;
				psKick->ahDstSyncInfo[ui32FenceSlot] = (IMG_HANDLE)sFenceBind.psSyncInfo;
				abDstSyncEnable[ui32FenceSlot] = IMG_TRUE;
				if (bAppend)
					psKick->ui32NumDstSync++;
				ui32RealDstSyncNum++;
			}
		}
#endif /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC) */
	}

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	if ((psKick->ui32Flags & SGXMKIF_TQFLAGS_KEEPPENDING) != 0UL &&
	    psKick->iFenceFd > 0)
	{
		int iRet;

		/*
		 * A KEEPPENDING transfer never attaches the M3 completion sync
		 * (the whole destination-sync section above is skipped).  A real
		 * core alloc must not be submitted without its fence: identify
		 * and claim it with the exact alloc-fops mechanism and fail the
		 * transfer pre-submit.  Only the TA/3D operations were taken
		 * (src/dst follow the KEEPPENDING semantics), so the shared
		 * unwind rolls back exactly those; the claim is dropped and the
		 * alloc stays reusable.  Legacy non-core fds keep the accepted
		 * stub behavior.
		 */
		iRet = PVRSyncProviderBindClaim((int)(psKick->iFenceFd - 1), &sFenceBind);
		if (iRet == -EINVAL)
		{
			/* not a core alloc fd: no dependency, proceed as before */
		}
		else if (iRet == 0)
		{
			PVRSyncProviderBindAbort(&sFenceBind);
			SGXTransferUnwindSyncOps(psKick, abSrcSyncEnable, abDstSyncEnable, IMG_NULL, IMG_FALSE);

			PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: KEEPPENDING transfer with a core alloc fd is not supported"));
			PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
					   TRANSFER_TOKEN_SUBMIT);
			return PVRSRV_ERROR_INVALID_PARAMS;
		}
		else
		{
			SGXTransferUnwindSyncOps(psKick, abSrcSyncEnable, abDstSyncEnable, IMG_NULL, IMG_FALSE);

			PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: M3 alloc claim failed (%d)", iRet));
			PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
					   TRANSFER_TOKEN_SUBMIT);
			return SGXTransferBindError(iRet);
		}
	}
#endif /* defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC) */

	psSharedTransferCmd->ui32NumSrcSyncs = ui32RealSrcSyncNum; 
	psSharedTransferCmd->ui32NumDstSyncs = ui32RealDstSyncNum; 

#if defined(PDUMP)
	if (PDumpWillCapture(psKick->ui32PDumpFlags))
	{
		PDUMPCOMMENT("Shared part of transfer command\r\n");
		PDUMPMEM(psSharedTransferCmd,
				psCCBMemInfo,
				psKick->ui32CCBDumpWOff,
				sizeof(SGXMKIF_TRANSFERCMD_SHARED),
				psKick->ui32PDumpFlags,
				MAKEUNIQUETAG(psCCBMemInfo));

		if ((psKick->ui32Flags & SGXMKIF_TQFLAGS_KEEPPENDING) == 0UL)
		{
			IMG_UINT32 i = 0;

			for (loop = 0; loop < psKick->ui32NumSrcSync; loop++)
			{
				if (abSrcSyncEnable[loop])
				{
					IMG_UINT32 ui32PDumpReadOp2 = 0;
					psSyncInfo = psKick->ahSrcSyncInfo[loop];

					PDUMPCOMMENT("Tweak src surface write op in transfer cmd\r\n");
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
					PDUMPCOMMENTWITHFLAGS(PDUMP_FLAGS_PERSISTENT, "TQ Src: PDump sync sample: uiAddr = 0x%08x, ui32LastOpDumpVal = 0x%08x\r\n",
											psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr,
											psSyncInfo->psSyncData->ui32LastOpDumpVal);
#endif
					PDUMPMEM(&psSyncInfo->psSyncData->ui32LastOpDumpVal,
							psCCBMemInfo,
							psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, asSrcSyncs) + i * sizeof(PVRSRV_DEVICE_SYNC_OBJECT) + offsetof(PVRSRV_DEVICE_SYNC_OBJECT, ui32WriteOpsPendingVal)),
							sizeof(psSyncInfo->psSyncData->ui32LastOpDumpVal),
							psKick->ui32PDumpFlags,
							MAKEUNIQUETAG(psCCBMemInfo));

					PDUMPCOMMENT("Tweak src surface read op in transfer cmd\r\n");
					PDUMPMEM(&psSyncInfo->psSyncData->ui32LastReadOpDumpVal,
							psCCBMemInfo,
							psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, asSrcSyncs) + i * sizeof(PVRSRV_DEVICE_SYNC_OBJECT) + offsetof(PVRSRV_DEVICE_SYNC_OBJECT, ui32ReadOpsPendingVal)),
							sizeof(psSyncInfo->psSyncData->ui32LastReadOpDumpVal),
							psKick->ui32PDumpFlags,
							MAKEUNIQUETAG(psCCBMemInfo));

					PDUMPCOMMENT("Tweak srv surface read op2 in transfer cmd\r\n");
					PDUMPMEM(&ui32PDumpReadOp2,
							psCCBMemInfo,
							psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, asSrcSyncs) + i * sizeof(PVRSRV_DEVICE_SYNC_OBJECT) + offsetof(PVRSRV_DEVICE_SYNC_OBJECT, ui32ReadOps2PendingVal)),
							sizeof(ui32PDumpReadOp2),
							psKick->ui32PDumpFlags,
							MAKEUNIQUETAG(psCCBMemInfo));
					i++;
				}
			}

			i = 0;
			for (loop = 0; loop < psKick->ui32NumDstSync; loop++)
			{
				/* the enable mask is indexed by the nominal iterator;
				 * i is only the compacted CCB destination index */
				if (abDstSyncEnable[loop])
				{
					IMG_UINT32 ui32PDumpReadOp2 = 0;
					psSyncInfo = psKick->ahDstSyncInfo[loop];

					PDUMPCOMMENT("Tweak dest surface write op in transfer cmd\r\n");
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
					PDUMPCOMMENTWITHFLAGS(PDUMP_FLAGS_PERSISTENT, "TQ Dst: PDump sync sample: uiAddr = 0x%08x, ui32LastOpDumpVal = 0x%08x\r\n",
											psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr,
											psSyncInfo->psSyncData->ui32LastOpDumpVal);
#endif
					PDUMPMEM(&psSyncInfo->psSyncData->ui32LastOpDumpVal,
							psCCBMemInfo,
							psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, asDstSyncs) + i * sizeof(PVRSRV_DEVICE_SYNC_OBJECT) + offsetof(PVRSRV_DEVICE_SYNC_OBJECT, ui32WriteOpsPendingVal)),
							sizeof(psSyncInfo->psSyncData->ui32LastOpDumpVal),
							psKick->ui32PDumpFlags,
							MAKEUNIQUETAG(psCCBMemInfo));

					PDUMPCOMMENT("Tweak dest surface read op in transfer cmd\r\n");
					PDUMPMEM(&psSyncInfo->psSyncData->ui32LastReadOpDumpVal,
							psCCBMemInfo,
							psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, asDstSyncs) + i * sizeof(PVRSRV_DEVICE_SYNC_OBJECT) + offsetof(PVRSRV_DEVICE_SYNC_OBJECT, ui32ReadOpsPendingVal)),
							sizeof(psSyncInfo->psSyncData->ui32LastReadOpDumpVal),
							psKick->ui32PDumpFlags,
							MAKEUNIQUETAG(psCCBMemInfo));

					PDUMPCOMMENT("Tweak dest surface read op2 in transfer cmd\r\n");
					PDUMPMEM(&ui32PDumpReadOp2,
							psCCBMemInfo,
							psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, asDstSyncs) + i * sizeof(PVRSRV_DEVICE_SYNC_OBJECT) + offsetof(PVRSRV_DEVICE_SYNC_OBJECT, ui32ReadOps2PendingVal)),
							sizeof(ui32PDumpReadOp2),
							psKick->ui32PDumpFlags,
							MAKEUNIQUETAG(psCCBMemInfo));
					i++;
				}
			}

			/*
			 * We allow the first source and destination sync objects to be the
			 * same, which is why the read/write pending updates are delayed
			 * until the transfer command has been updated with the current
			 * values from the objects.
			 */
			for (loop = 0; loop < (psKick->ui32NumSrcSync); loop++)
			{
				if (abSrcSyncEnable[loop])
				{	
					psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[loop];
					psSyncInfo->psSyncData->ui32LastReadOpDumpVal++;
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
					PDUMPCOMMENTWITHFLAGS(PDUMP_FLAGS_PERSISTENT,
											"TQ Src: PDump sync update: uiAddr = 0x%08x, ui32LastReadOpDumpVal = 0x%08x\r\n",
											psSyncInfo->sReadOpsCompleteDevVAddr.uiAddr,
											psSyncInfo->psSyncData->ui32LastReadOpDumpVal);
#endif
				}
			}

			for (loop = 0; loop < (psKick->ui32NumDstSync); loop++)
			{
				if (abDstSyncEnable[loop])
				{
					psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahDstSyncInfo[loop];
					psSyncInfo->psSyncData->ui32LastOpDumpVal++;
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
					PDUMPCOMMENTWITHFLAGS(PDUMP_FLAGS_PERSISTENT,
											"TQ Dst: PDump sync update: uiAddr = 0x%08x, ui32LastOpDumpVal = 0x%08x\r\n",
											psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr,
											psSyncInfo->psSyncData->ui32LastOpDumpVal);
#endif
				}
			}
		}

		if (psKick->hTASyncInfo != IMG_NULL)
		{
			psSyncInfo = psKick->hTASyncInfo;

			PDUMPCOMMENT("Tweak TA/TQ surface write op in transfer cmd\r\n");
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
			PDUMPCOMMENT("TQ TA/TQ: PDump sync sample: uiAddr = 0x%08x, ui32LastOpDumpVal = 0x%08x\r\n",
									psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr,
									psSyncInfo->psSyncData->ui32LastOpDumpVal);
#endif
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, ui32TASyncWriteOpsPendingVal)),
					sizeof(psSyncInfo->psSyncData->ui32LastOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));

			PDUMPCOMMENT("Tweak TA/TQ surface read op in transfer cmd\r\n");
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastReadOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, ui32TASyncReadOpsPendingVal)),
					sizeof(psSyncInfo->psSyncData->ui32LastReadOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));

			psSyncInfo->psSyncData->ui32LastOpDumpVal++;
		}

		if (psKick->h3DSyncInfo != IMG_NULL)
		{
			psSyncInfo = psKick->h3DSyncInfo;

			PDUMPCOMMENT("Tweak 3D/TQ surface write op in transfer cmd\r\n");
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
			PDUMPCOMMENT("TQ 3D/TQ: PDump sync sample: uiAddr = 0x%08x, ui32LastOpDumpVal = 0x%08x\r\n",
									psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr,
									psSyncInfo->psSyncData->ui32LastOpDumpVal);
#endif
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, ui323DSyncWriteOpsPendingVal)),
					sizeof(psSyncInfo->psSyncData->ui32LastOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));

			PDUMPCOMMENT("Tweak 3D/TQ surface read op in transfer cmd\r\n");
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastReadOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)(offsetof(SGXMKIF_TRANSFERCMD_SHARED, ui323DSyncReadOpsPendingVal)),
					sizeof(psSyncInfo->psSyncData->ui32LastReadOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));

			psSyncInfo->psSyncData->ui32LastOpDumpVal++;
		}
	}
#endif

	sCommand.ui32Data[1] = psKick->sHWTransferContextDevVAddr.uiAddr;
	
	PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_CMD_END,
			TRANSFER_TOKEN_SUBMIT);

	eError = SGXScheduleCCBCommandKM(hDevHandle, SGXMKIF_CMD_TRANSFER, &sCommand, KERNEL_ID, psKick->ui32PDumpFlags, hDevMemContext, IMG_FALSE);

	if (eError == PVRSRV_ERROR_RETRY)
	{
		/*
		 * M3: RETRY is the power-lock-busy path of
		 * SGXScheduleCCBCommandKM(); it is a proven pre-submit failure
		 * (nothing was handed to the UKernel).  Unwind every taken sync
		 * operation through the shared pre-submit routine and drop the
		 * binding/claim; the client may retry the same alloc fd.
		 */
		SGXTransferUnwindSyncOps(psKick, abSrcSyncEnable, abDstSyncEnable,
					 psFenceSyncInfo, IMG_TRUE);
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
		PVRSyncProviderBindAbort(&sFenceBind);
#endif
	}
	else if (PVRSRV_OK != eError)
	{
		/*
		 * M3: every non-OK return of SGXScheduleCCBCommandKM() is a proven
		 * pre-submit failure (power lock/power-up/CCB slot/command
		 * validation/kick; full source evidence in
		 * notes/l11-m3-provider-20260914/scheduler-contract.md).  The same
		 * shared routine unwinds every taken operation exactly once and the
		 * binding/claim is dropped; no token was accepted.
		 */
		SGXTransferUnwindSyncOps(psKick, abSrcSyncEnable, abDstSyncEnable,
					 psFenceSyncInfo, IMG_TRUE);
#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
		PVRSyncProviderBindAbort(&sFenceBind);
#endif

		PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: SGXScheduleCCBCommandKM failed."));
		PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
				TRANSFER_TOKEN_SUBMIT);
		return eError;
	}

#if defined(PVR_ANDROID_NATIVE_WINDOW_HAS_SYNC)
	/*
	 * M3: the submit succeeded, so the prepared completion object is now
	 * driven by the GPU.  Bind it to the ALLOC fd; userspace's CREATE is
	 * the next step.  A bind failure (misuse/concurrent consumption) leaves
	 * the alloc unbound and the object is kept alive until completion.
	 */
	if (PVRSyncProviderBindCommit(&sFenceBind) != 0)
	{
		PVR_DPF((PVR_DBG_ERROR, "SGXSubmitTransferKM: provider bind commit failed "
					"(work submitted, fence stays unbound)"));
	}
#endif
	

#if defined(NO_HARDWARE)
	if ((psKick->ui32Flags & SGXMKIF_TQFLAGS_NOSYNCUPDATE) == 0)
	{
		/* Update sync objects pretending that we have done the job*/
		for (loop = 0; loop < psKick->ui32NumSrcSync; loop++)
		{
			if (abSrcSyncEnable[loop])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[loop];
				psSyncInfo->psSyncData->ui32ReadOpsComplete = psSyncInfo->psSyncData->ui32ReadOpsPending;
			}
		}

		for (loop = 0; loop < psKick->ui32NumDstSync; loop++)
		{
			if (abDstSyncEnable[loop])
			{
				psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahDstSyncInfo[loop];
				psSyncInfo->psSyncData->ui32WriteOpsComplete = psSyncInfo->psSyncData->ui32WriteOpsPending;
			}
		}

		if (psKick->hTASyncInfo != IMG_NULL)
		{
			psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->hTASyncInfo;

			psSyncInfo->psSyncData->ui32WriteOpsComplete = psSyncInfo->psSyncData->ui32WriteOpsPending;
		}

		if (psKick->h3DSyncInfo != IMG_NULL)
		{
			psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->h3DSyncInfo;

			psSyncInfo->psSyncData->ui32WriteOpsComplete = psSyncInfo->psSyncData->ui32WriteOpsPending;
		}
	}
#endif
	PVR_TTRACE(PVRSRV_TRACE_GROUP_TRANSFER, PVRSRV_TRACE_CLASS_FUNCTION_EXIT,
			TRANSFER_TOKEN_SUBMIT);
	return eError;
}

#if defined(SGX_FEATURE_2D_HARDWARE)
IMG_EXPORT PVRSRV_ERROR SGXSubmit2DKM(IMG_HANDLE hDevHandle, PVRSRV_2D_SGX_KICK *psKick)
					    
{
	PVRSRV_KERNEL_MEM_INFO  *psCCBMemInfo = (PVRSRV_KERNEL_MEM_INFO *)psKick->hCCBMemInfo;
	SGXMKIF_COMMAND sCommand = {0};
	SGXMKIF_2DCMD_SHARED *ps2DCmd;
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo;
	PVRSRV_ERROR eError;
	IMG_UINT32 i;
	IMG_HANDLE hDevMemContext = IMG_NULL;
#if defined(FIX_HW_BRN_31620)
	hDevMemContext = psKick->hDevMemContext;
#endif

	if (!CCB_OFFSET_IS_VALID(SGXMKIF_2DCMD_SHARED, psCCBMemInfo, psKick, ui32SharedCmdCCBOffset))
	{
		PVR_DPF((PVR_DBG_ERROR, "SGXSubmit2DKM: Invalid CCB offset"));
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	/* override QAC warning about stricter alignment */
	/* PRQA S 3305 1 */
	ps2DCmd =  CCB_DATA_FROM_OFFSET(SGXMKIF_2DCMD_SHARED, psCCBMemInfo, psKick, ui32SharedCmdCCBOffset);

	/* Command needs to be synchronised with the TA? */
	if (psKick->hTASyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->hTASyncInfo;

		ps2DCmd->sTASyncData.ui32WriteOpsPendingVal = SyncTakeWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);
		ps2DCmd->sTASyncData.ui32ReadOpsPendingVal = psSyncInfo->psSyncData->ui32ReadOpsPending;

		ps2DCmd->sTASyncData.sWriteOpsCompleteDevVAddr 	= psSyncInfo->sWriteOpsCompleteDevVAddr;
		ps2DCmd->sTASyncData.sReadOpsCompleteDevVAddr 	= psSyncInfo->sReadOpsCompleteDevVAddr;
	}
	else
	{
		ps2DCmd->sTASyncData.sWriteOpsCompleteDevVAddr.uiAddr = 0;
		ps2DCmd->sTASyncData.sReadOpsCompleteDevVAddr.uiAddr = 0;
	}

	/* Command needs to be synchronised with the 3D? */
	if (psKick->h3DSyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->h3DSyncInfo;

		ps2DCmd->s3DSyncData.ui32WriteOpsPendingVal = SyncTakeWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);	
		ps2DCmd->s3DSyncData.ui32ReadOpsPendingVal = psSyncInfo->psSyncData->ui32ReadOpsPending;

		ps2DCmd->s3DSyncData.sWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
		ps2DCmd->s3DSyncData.sReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
	}
	else
	{
		ps2DCmd->s3DSyncData.sWriteOpsCompleteDevVAddr.uiAddr = 0;
		ps2DCmd->s3DSyncData.sReadOpsCompleteDevVAddr.uiAddr = 0;
	}

	/*
	 * We allow the first source and destination sync objects to be the
	 * same, which is why the read/write pending updates are delayed
	 * until the transfer command has been updated with the current
	 * values from the objects.
	 */
	ps2DCmd->ui32NumSrcSync = psKick->ui32NumSrcSync;

	for (i = 0; i < psKick->ui32NumSrcSync; i++)
	{
		psSyncInfo = psKick->ahSrcSyncInfo[i];

		ps2DCmd->sSrcSyncData[i].ui32WriteOpsPendingVal = psSyncInfo->psSyncData->ui32WriteOpsPending;
		ps2DCmd->sSrcSyncData[i].ui32ReadOpsPendingVal = psSyncInfo->psSyncData->ui32ReadOpsPending;

		ps2DCmd->sSrcSyncData[i].sWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
		ps2DCmd->sSrcSyncData[i].sReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
	}

	if (psKick->hDstSyncInfo != IMG_NULL)
	{
		psSyncInfo = psKick->hDstSyncInfo;

		ps2DCmd->sDstSyncData.ui32WriteOpsPendingVal = psSyncInfo->psSyncData->ui32WriteOpsPending;
		ps2DCmd->sDstSyncData.ui32ReadOpsPendingVal = psSyncInfo->psSyncData->ui32ReadOpsPending;
		ps2DCmd->sDstSyncData.ui32ReadOps2PendingVal = psSyncInfo->psSyncData->ui32ReadOps2Pending;

		ps2DCmd->sDstSyncData.sWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
		ps2DCmd->sDstSyncData.sReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
		ps2DCmd->sDstSyncData.sReadOps2CompleteDevVAddr = psSyncInfo->sReadOps2CompleteDevVAddr;

		/* We can do this immediately as we only have one */
		SyncTakeWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);
	}
	else
	{
		ps2DCmd->sDstSyncData.sWriteOpsCompleteDevVAddr.uiAddr = 0;
		ps2DCmd->sDstSyncData.sReadOpsCompleteDevVAddr.uiAddr = 0;
		ps2DCmd->sDstSyncData.sReadOps2CompleteDevVAddr.uiAddr = 0;
	}

	/* Read/Write ops pending updates, delayed from above */
	for (i = 0; i < psKick->ui32NumSrcSync; i++)
	{
		psSyncInfo = psKick->ahSrcSyncInfo[i];
		SyncTakeReadOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);
	}

#if defined(PDUMP)
	if (PDumpWillCapture(psKick->ui32PDumpFlags))
	{
		/* Pdump the command from the per context CCB */
		PDUMPCOMMENT("Shared part of 2D command\r\n");
		PDUMPMEM(ps2DCmd,
				psCCBMemInfo,
				psKick->ui32CCBDumpWOff,
				sizeof(SGXMKIF_2DCMD_SHARED),
				psKick->ui32PDumpFlags,
				MAKEUNIQUETAG(psCCBMemInfo));

		for (i = 0; i < psKick->ui32NumSrcSync; i++)
		{
			psSyncInfo = psKick->ahSrcSyncInfo[i];

			PDUMPCOMMENT("Tweak src surface write op in 2D cmd\r\n");
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)offsetof(SGXMKIF_2DCMD_SHARED, sSrcSyncData[i].ui32WriteOpsPendingVal),
					sizeof(psSyncInfo->psSyncData->ui32LastOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));

			PDUMPCOMMENT("Tweak src surface read op in 2D cmd\r\n");
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastReadOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)offsetof(SGXMKIF_2DCMD_SHARED, sSrcSyncData[i].ui32ReadOpsPendingVal),
					sizeof(psSyncInfo->psSyncData->ui32LastReadOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));
		}

		if (psKick->hDstSyncInfo != IMG_NULL)
		{
			IMG_UINT32 ui32PDumpReadOp2 = 0;
			psSyncInfo = psKick->hDstSyncInfo;

			PDUMPCOMMENT("Tweak dest surface write op in 2D cmd\r\n");
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)offsetof(SGXMKIF_2DCMD_SHARED, sDstSyncData.ui32WriteOpsPendingVal),
					sizeof(psSyncInfo->psSyncData->ui32LastOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));

			PDUMPCOMMENT("Tweak dest surface read op in 2D cmd\r\n");
			PDUMPMEM(&psSyncInfo->psSyncData->ui32LastReadOpDumpVal,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)offsetof(SGXMKIF_2DCMD_SHARED, sDstSyncData.ui32ReadOpsPendingVal),
					sizeof(psSyncInfo->psSyncData->ui32LastReadOpDumpVal),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));
			PDUMPCOMMENT("Tweak dest surface read op2 in 2D cmd\r\n");
			PDUMPMEM(&ui32PDumpReadOp2,
					psCCBMemInfo,
					psKick->ui32CCBDumpWOff + (IMG_UINT32)offsetof(SGXMKIF_2DCMD_SHARED, sDstSyncData.ui32ReadOps2PendingVal),
					sizeof(ui32PDumpReadOp2),
					psKick->ui32PDumpFlags,
					MAKEUNIQUETAG(psCCBMemInfo));
		}

		/* Read/Write ops pending updates, delayed from above */
		for (i = 0; i < psKick->ui32NumSrcSync; i++)
		{
			psSyncInfo = psKick->ahSrcSyncInfo[i];
			psSyncInfo->psSyncData->ui32LastReadOpDumpVal++;
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
			PDUMPCOMMENTWITHFLAGS(PDUMP_FLAGS_PERSISTENT,
									"TQ2D Src: PDump sync update: uiAddr = 0x%08x, ui32LastReadOpDumpVal = 0x%08x\r\n",
									psSyncInfo->sReadOpsCompleteDevVAddr.uiAddr,
									psSyncInfo->psSyncData->ui32LastReadOpDumpVal);
#endif
		}

		if (psKick->hDstSyncInfo != IMG_NULL)
		{
			psSyncInfo = psKick->hDstSyncInfo;
			psSyncInfo->psSyncData->ui32LastOpDumpVal++;
#if defined(SUPPORT_PDUMP_SYNC_DEBUG)
			PDUMPCOMMENTWITHFLAGS(PDUMP_FLAGS_PERSISTENT,
									"TQ2D Dst: PDump sync update: uiAddr = 0x%08x, ui32LastOpDumpVal = 0x%08x\r\n",
									psSyncInfo->sWriteOpsCompleteDevVAddr.uiAddr,
									psSyncInfo->psSyncData->ui32LastOpDumpVal);
#endif
		}
	}		
#endif

	sCommand.ui32Data[1] = psKick->sHW2DContextDevVAddr.uiAddr;
	
	eError = SGXScheduleCCBCommandKM(hDevHandle, SGXMKIF_CMD_2D, &sCommand, KERNEL_ID, psKick->ui32PDumpFlags, hDevMemContext, IMG_FALSE);	

	if (eError == PVRSRV_ERROR_RETRY)
	{
		/* Client will retry, so undo the write ops pending increment
		   done above.
		 */
#if defined(PDUMP)
		if (PDumpIsCaptureFrameKM())
		{
			for (i = 0; i < psKick->ui32NumSrcSync; i++)
			{
				psSyncInfo = psKick->ahSrcSyncInfo[i];
				psSyncInfo->psSyncData->ui32LastReadOpDumpVal--;
			}

			if (psKick->hDstSyncInfo != IMG_NULL)
			{
				psSyncInfo = psKick->hDstSyncInfo;
				psSyncInfo->psSyncData->ui32LastOpDumpVal--;
			}
		}
#endif

		for (i = 0; i < psKick->ui32NumSrcSync; i++)
		{
			psSyncInfo = psKick->ahSrcSyncInfo[i];
			SyncRollBackReadOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);
		}

		if (psKick->hDstSyncInfo != IMG_NULL)
		{
			psSyncInfo = psKick->hDstSyncInfo;
			SyncRollBackWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);
		}

		/* Command needed to be synchronised with the TA? */
		if (psKick->hTASyncInfo != IMG_NULL)
		{
			psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->hTASyncInfo;

			SyncRollBackWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);
		}

		/* Command needed to be synchronised with the 3D? */
		if (psKick->h3DSyncInfo != IMG_NULL)
		{
			psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->h3DSyncInfo;

			SyncRollBackWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_2D);
		}
	}

	


#if defined(NO_HARDWARE)
	/* Update sync objects pretending that we have done the job*/
	for(i = 0; i < psKick->ui32NumSrcSync; i++)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->ahSrcSyncInfo[i];
		psSyncInfo->psSyncData->ui32ReadOpsComplete = psSyncInfo->psSyncData->ui32ReadOpsPending;
	}

	if (psKick->hDstSyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->hDstSyncInfo;

		psSyncInfo->psSyncData->ui32WriteOpsComplete = psSyncInfo->psSyncData->ui32WriteOpsPending;
	}

	if (psKick->hTASyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->hTASyncInfo;

		psSyncInfo->psSyncData->ui32WriteOpsComplete = psSyncInfo->psSyncData->ui32WriteOpsPending;
	}

	if (psKick->h3DSyncInfo != IMG_NULL)
	{
		psSyncInfo = (PVRSRV_KERNEL_SYNC_INFO *)psKick->h3DSyncInfo;

		psSyncInfo->psSyncData->ui32WriteOpsComplete = psSyncInfo->psSyncData->ui32WriteOpsPending;
	}
#endif

	return eError;
}
#endif	/* SGX_FEATURE_2D_HARDWARE */
#endif	/* TRANSFER_QUEUE */
