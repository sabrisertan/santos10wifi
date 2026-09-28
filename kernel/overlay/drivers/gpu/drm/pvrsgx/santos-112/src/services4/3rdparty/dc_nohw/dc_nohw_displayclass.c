/*************************************************************************/ /*!
@Title          NOHW display driver display-specific functions
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

/**************************************************************************
 The 3rd party driver is a specification of an API to integrate the IMG POWERVR
 Services driver with 3rd Party display hardware.  It is NOT a specification for
 a display controller driver, rather a specification to extend the API for a
 pre-existing driver for the display hardware.

 The 3rd party driver interface provides IMG POWERVR client drivers (e.g. PVR2D)
 with an API abstraction of the system's underlying display hardware, allowing
 the client drivers to indirectly control the display hardware and access its
 associated memory.

 Functions of the API include
 - query primary surface attributes (width, height, stride, pixel format, CPU
     physical and virtual address)
 - swap/flip chain creation and subsequent query of surface attributes
 - asynchronous display surface flipping, taking account of asynchronous read
 (flip) and write (render) operations to the display surface

 Note: having queried surface attributes the client drivers are able to map the
 display memory to any IMG POWERVR Services device by calling
 PVRSRVMapDeviceClassMemory with the display surface handle.

 This code is intended to be an example of how a pre-existing display driver may
 be extended to support the 3rd Party Display interface to POWERVR Services
 - IMG is not providing a display driver implementation.
 **************************************************************************/

#if defined(__linux__)
#include <linux/string.h>
#else
#include <string.h>
#endif

#if defined(SANTOS10_DC_NOHW_STANDALONE)
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/barrier.h>
#endif

/* IMG services headers */
#include "img_defs.h"
#include "servicesext.h"
#include "kerneldisplay.h"
#include "servicesint.h"
#include "ra.h"
#include "device.h"
#include "buffer_manager.h"
#include "refcount.h"

#include "dc_nohw.h"
#include "mm.h"
#include "queue.h"

#define DISPLAY_DEVICE_NAME "DC_NOHW"

#define DC_NOHW_COMMAND_COUNT		1

/* top level 'hook ptr' */
static void *gpvAnchor = 0;
static PFN_DC_GET_PVRJTABLE pfnGetPVRJTable = 0;




/*
	Kernel services is a kernel module and must be loaded first.
	The display controller driver is also a kernel module and must be loaded after the pvr services module.
	The display controller driver should be able to retrieve the
	address of the services PVRGetDisplayClassJTable from (the already loaded)
	kernel services module.
*/

/* returns anchor pointer */
static DC_NOHW_DEVINFO * GetAnchorPtr(void)
{
	return (DC_NOHW_DEVINFO *)gpvAnchor;
}

/* sets anchor pointer */
static void SetAnchorPtr(DC_NOHW_DEVINFO *psDevInfo)
{
	gpvAnchor = (void *)psDevInfo;
}

#if !defined(DC_NOHW_DISCONTIG_BUFFERS) && !defined(USE_BASE_VIDEO_FRAMEBUFFER)
IMG_SYS_PHYADDR CpuPAddrToSysPAddr(IMG_CPU_PHYADDR cpu_paddr)
{
	IMG_SYS_PHYADDR sys_paddr;

	/* This would only be an inequality if the CPU's MMU did not point to sys address 0,
	   ie. multi CPU system */
	sys_paddr.uiAddr = cpu_paddr.uiAddr;
	return sys_paddr;
}

IMG_CPU_PHYADDR SysPAddrToCpuPAddr(IMG_SYS_PHYADDR sys_paddr)
{
	IMG_CPU_PHYADDR cpu_paddr;

	/* This would only be an inequality if the CPU's MMU did not point to sys address 0,
	   ie. multi CPU system */
	cpu_paddr.uiAddr = sys_paddr.uiAddr;
	return cpu_paddr;
}
#endif


static PVRSRV_ERROR OpenDCDevice(IMG_UINT32 ui32DeviceID,
                                 IMG_HANDLE *phDevice,
                                 PVRSRV_SYNC_DATA* psSystemBufferSyncData)
{
	DC_NOHW_DEVINFO *psDevInfo;
	PVR_UNREFERENCED_PARAMETER(ui32DeviceID);

	psDevInfo = GetAnchorPtr();

#if defined (ENABLE_DISPLAY_MODE_TRACKING)
	if (Shadow_Desktop_Resolution(psDevInfo) != DC_OK)
	{
		return (PVRSRV_ERROR_NOT_SUPPORTED);
	}
#endif

	/* store the system surface sync data */
	psDevInfo->asBackBuffers[0].psSyncData = psSystemBufferSyncData;

	/* return handle to the devinfo */
	*phDevice = (IMG_HANDLE)psDevInfo;

#if defined(USE_BASE_VIDEO_FRAMEBUFFER)
	return (SetupDevInfo(psDevInfo));
#else
	return (PVRSRV_OK);
#endif
}


static PVRSRV_ERROR CloseDCDevice(IMG_HANDLE hDevice)
{
	UNREFERENCED_PARAMETER(hDevice);

#if defined(USE_BASE_VIDEO_FRAMEBUFFER)
	FreeBackBuffers(GetAnchorPtr());
#endif

	return (PVRSRV_OK);
}


static PVRSRV_ERROR EnumDCFormats(IMG_HANDLE		hDevice,
                                  IMG_UINT32		*pui32NumFormats,
                                  DISPLAY_FORMAT	*psFormat)
{
	DC_NOHW_DEVINFO	*psDevInfo;

	if(!hDevice || !pui32NumFormats)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (DC_NOHW_DEVINFO *)hDevice;

	*pui32NumFormats = (IMG_UINT32)psDevInfo->ulNumFormats;

	if(psFormat != IMG_NULL)
	{
		unsigned long i;

		for(i=0; i<psDevInfo->ulNumFormats; i++)
		{
			psFormat[i] = psDevInfo->asDisplayFormatList[i];
		}
	}

	return (PVRSRV_OK);
}


static PVRSRV_ERROR EnumDCDims(IMG_HANDLE		hDevice,
                               DISPLAY_FORMAT	*psFormat,
                               IMG_UINT32		*pui32NumDims,
                               DISPLAY_DIMS		*psDim)
{
	DC_NOHW_DEVINFO	*psDevInfo;

	if(!hDevice || !psFormat || !pui32NumDims)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (DC_NOHW_DEVINFO *)hDevice;

	*pui32NumDims = (IMG_UINT32)psDevInfo->ulNumDims;

	/* given psFormat return the available Dims */

	if(psDim != IMG_NULL)
	{
		unsigned long i;

		for(i=0; i<psDevInfo->ulNumDims; i++)
		{
			psDim[i] = psDevInfo->asDisplayDimList[i];
		}
	}

	return (PVRSRV_OK);
}


static PVRSRV_ERROR GetDCSystemBuffer(IMG_HANDLE hDevice, IMG_HANDLE *phBuffer)
{
	DC_NOHW_DEVINFO	*psDevInfo;

	if(!hDevice || !phBuffer)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (DC_NOHW_DEVINFO *)hDevice;

	*phBuffer = (IMG_HANDLE)&psDevInfo->asBackBuffers[0];

	return (PVRSRV_OK);
}


static PVRSRV_ERROR GetDCInfo(IMG_HANDLE hDevice, DISPLAY_INFO *psDCInfo)
{
	DC_NOHW_DEVINFO	*psDevInfo;

	if(!hDevice || !psDCInfo)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (DC_NOHW_DEVINFO *)hDevice;

	*psDCInfo = psDevInfo->sDisplayInfo;

	return (PVRSRV_OK);
}


static PVRSRV_ERROR GetDCBufferAddr(IMG_HANDLE         hDevice,
                                    IMG_HANDLE         hBuffer,
                                    IMG_SYS_PHYADDR **ppsSysAddr,
                                    IMG_UINT32        *pui32ByteSize,
                                    IMG_VOID         **ppvCpuVAddr,
                                    IMG_HANDLE        *phOSMapInfo,
                                    IMG_BOOL          *pbIsContiguous,
                                    IMG_UINT32		  *pui32TilingStride)
{
	DC_NOHW_DEVINFO	*psDevInfo;
	DC_NOHW_BUFFER	*psBuffer;

	if(!hDevice || !hBuffer || !ppsSysAddr || !pui32ByteSize)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (DC_NOHW_DEVINFO *)hDevice;

	psBuffer = (DC_NOHW_BUFFER*)hBuffer;

	*ppvCpuVAddr = psBuffer->sCPUVAddr;

	*pui32ByteSize = (IMG_UINT32)(psDevInfo->asDisplayDimList[0].ui32Height * psDevInfo->asDisplayDimList[0].ui32ByteStride);
	*phOSMapInfo = IMG_NULL;

#if defined(DC_NOHW_DISCONTIG_BUFFERS)
	*ppsSysAddr = psBuffer->psSysAddr;
	*pbIsContiguous = IMG_FALSE;
#else
	*ppsSysAddr = &psBuffer->sSysAddr;
	*pbIsContiguous = IMG_TRUE;
#endif

#if defined(SUPPORT_MEMORY_TILING)
	{
		IMG_UINT32 ui32Stride = psDevInfo->asDisplayDimList[0].ui32ByteStride;
		IMG_UINT32 ui32NumBits = 0, ui32StrideTopBit, n;

		// How many bits for x?
		for(n = 0; n < 32; n++)
		{
			if(ui32Stride & (1<<n))
			{
				ui32NumBits = n+1;
			}
		}

		// clamp to the minimum..
		if(ui32NumBits < 10)
		{
			ui32NumBits = 10;
		}

		// Subtract one to make this a range limit..
		ui32StrideTopBit = ui32NumBits - 1;

		// Subtract 9 to prepare it for the HW..
		ui32StrideTopBit -= 9;

		*pui32TilingStride = ui32StrideTopBit;
	}
#else
	UNREFERENCED_PARAMETER(pui32TilingStride);
#endif /* defined(SUPPORT_MEMORY_TILING) */

	return (PVRSRV_OK);
}

#if defined(SANTOS10_DC_NOHW_STANDALONE)
static DEFINE_SPINLOCK(sA151fLock);
static IMG_BOOL gbA151fShuttingDown = IMG_FALSE;

static void santos_a151f_on_create_swapchain(void)
{
	unsigned long flags;

	spin_lock_irqsave(&sA151fLock, flags);
	gbA151fShuttingDown = IMG_FALSE;
	spin_unlock_irqrestore(&sA151fLock, flags);
}
#endif

static PVRSRV_ERROR CreateDCSwapChain(IMG_HANDLE hDevice,
                                      IMG_UINT32 ui32Flags,
                                      DISPLAY_SURF_ATTRIBUTES *psDstSurfAttrib,
                                      DISPLAY_SURF_ATTRIBUTES *psSrcSurfAttrib,
                                      IMG_UINT32 ui32BufferCount,
                                      PVRSRV_SYNC_DATA **ppsSyncData,
                                      IMG_UINT32 ui32OEMFlags,
                                      IMG_HANDLE *phSwapChain,
                                      IMG_UINT32 *pui32SwapChainID)
{
	DC_NOHW_DEVINFO	*psDevInfo;
	DC_NOHW_SWAPCHAIN *psSwapChain;
	DC_NOHW_BUFFER *psBuffer;
	IMG_UINT32 i;

	UNREFERENCED_PARAMETER(ui32OEMFlags);
	UNREFERENCED_PARAMETER(pui32SwapChainID);

	/* check parameters */
	if(!hDevice
	|| !psDstSurfAttrib
	|| !psSrcSurfAttrib
	|| !ppsSyncData
	|| !phSwapChain)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (DC_NOHW_DEVINFO*)hDevice;

	/* the dc_nohw only supports a single swapchain */
	if(psDevInfo->psSwapChain)
	{
		return (PVRSRV_ERROR_FLIP_CHAIN_EXISTS);
	}
	
	/* create a swapchain structure */
	psSwapChain = (DC_NOHW_SWAPCHAIN*)AllocKernelMem(sizeof(DC_NOHW_SWAPCHAIN));
	if(!psSwapChain)
	{
		return (PVRSRV_ERROR_OUT_OF_MEMORY);
	}
	
	memset(psSwapChain, 0, sizeof(DC_NOHW_SWAPCHAIN));

	if (ui32BufferCount)
	{
	
		/* check the buffer count */
		if(ui32BufferCount > DC_NOHW_MAX_BACKBUFFERS)
		{
			return (PVRSRV_ERROR_TOOMANYBUFFERS);
		}
	
		/*
			verify the DST/SRC attributes
			- SRC/DST must match the current display mode config
		*/
		if(psDstSurfAttrib->pixelformat != psDevInfo->sSysFormat.pixelformat
		|| psDstSurfAttrib->sDims.ui32ByteStride != psDevInfo->sSysDims.ui32ByteStride
		|| psDstSurfAttrib->sDims.ui32Width != psDevInfo->sSysDims.ui32Width
		|| psDstSurfAttrib->sDims.ui32Height != psDevInfo->sSysDims.ui32Height)
		{
			/* DST doesn't match the current mode */
			return (PVRSRV_ERROR_INVALID_PARAMS);
		}
	
		if(psDstSurfAttrib->pixelformat != psSrcSurfAttrib->pixelformat
		|| psDstSurfAttrib->sDims.ui32ByteStride != psSrcSurfAttrib->sDims.ui32ByteStride
		|| psDstSurfAttrib->sDims.ui32Width != psSrcSurfAttrib->sDims.ui32Width
		|| psDstSurfAttrib->sDims.ui32Height != psSrcSurfAttrib->sDims.ui32Height)
		{
			/* DST doesn't match the SRC */
			return (PVRSRV_ERROR_INVALID_PARAMS);
		}
	
		/* INTEGRATION_POINT: check the flags */
		UNREFERENCED_PARAMETER(ui32Flags);
	
	
	
		psBuffer = (DC_NOHW_BUFFER*)AllocKernelMem(sizeof(DC_NOHW_BUFFER) * ui32BufferCount);
		if(!psBuffer)
		{
			FreeKernelMem(psSwapChain);
			return (PVRSRV_ERROR_OUT_OF_MEMORY);
		}
	
		/* initialise allocations */
		memset(psBuffer, 0, sizeof(DC_NOHW_BUFFER) * ui32BufferCount);
	
		psSwapChain->ulBufferCount = (unsigned long)ui32BufferCount;
		psSwapChain->psBuffer = psBuffer;
	
		/* link the buffers */
		for(i=0; i<ui32BufferCount-1; i++)
		{
			psBuffer[i].psNext = &psBuffer[i+1];
		}
		/* and link last to first */
		psBuffer[i].psNext = &psBuffer[0];
	
		/* populate the buffers */
		for(i=0; i<ui32BufferCount; i++)
		{
			psBuffer[i].psSyncData = ppsSyncData[i];
#if defined(DC_NOHW_DISCONTIG_BUFFERS)
			psBuffer[i].psSysAddr = psDevInfo->asBackBuffers[i].psSysAddr;
#else
			psBuffer[i].sSysAddr = psDevInfo->asBackBuffers[i].sSysAddr;
#endif
			psBuffer[i].sDevVAddr = psDevInfo->asBackBuffers[i].sDevVAddr;
			psBuffer[i].sCPUVAddr = psDevInfo->asBackBuffers[i].sCPUVAddr;
			psBuffer[i].hSwapChain = (DC_HANDLE)psSwapChain;
		}
	}
	else
	{
		psSwapChain->psBuffer = NULL;
	}

	/* mark swapchain's existence */
	psDevInfo->psSwapChain = psSwapChain;

	/* return swapchain handle */
	*phSwapChain = (IMG_HANDLE)psSwapChain;

	/* INTEGRATION_POINT: enable Vsync ISR */

#if defined(SANTOS10_DC_NOHW_STANDALONE)
	santos_a151f_on_create_swapchain();
#endif

	return (PVRSRV_OK);
}


/*
 * A151f emergency CPU-copy presentation path (fallback only; Candidate 3B
 * r8r2 remains the primary zero-copy scanout).
 *
 * Gates: exactly one posted meminfo, byte size exactly 0x3e8000,
 * LINUX_MEM_AREA_ALLOC_PAGES, WRITECOMBINE allocation flags, valid 1000-page
 * backing list.  The backing LinuxMemArea must be PAT WC_TRACKED (see
 * LinuxMemAreaPatEnsureWC): the WC transition happens before the first WC
 * userspace PTE (mmap step 0) or here before the A151f WC vmap, and the area
 * stays WC_TRACKED until its final backing destruction.  A151f uses a single
 * WC vmap and reads it with memcpy_toio (historical ~50 ms class); the old
 * dual WC/WB vmap benchmark and the per-frame drm_clflush WB fast path are
 * removed because they published conflicting PAT aliases.  The simplefb
 * destination (physical 0x3f000000) is ioremap_wc()ed once to match Linux
 * simplefb's mapping.  There is no copy cap; a heartbeat every 1000 copies
 * reports the cached source-map count.  Mappings are released when the owning
 * BM context is destroyed (L58) and by the global DeInit teardown; slot
 * eviction vunmaps only and never touches the area's PAT state.
 */
#define SANTOS_A151F_SIZE	0x3e8000u
#define SANTOS_A151F_PAGES	(SANTOS_A151F_SIZE / PAGE_SIZE)
#define SANTOS_A151F_FB_PHYS	0x3f000000u
#define SANTOS_A151F_MAX_SRC	8

struct santos_a151f_src {
	PDC_MEM_INFO		psMemInfo;
	PPVRSRV_KERNEL_MEM_INFO	psKMemInfo;
	LinuxMemArea		*psArea;
	struct page		**ppsPages;
	struct page		*pFirstPage;
	IMG_UINT32		ui32DevVAddr;
	void			*pvMapWc;
	IMG_HANDLE		hOwnerBMContext;
	IMG_BOOL		bInUse;
	IMG_UINT64		ui64LastUsed;
};

static struct santos_a151f_src gasA151fSrc[SANTOS_A151F_MAX_SRC];
static void __iomem *gpvA151fFb;
static IMG_UINT32 gui32A151fCopies;
static IMG_UINT32 gui32A151fFailedCopies;
static IMG_UINT32 gui32A151fDrainedFlips;
static IMG_UINT32 gui32A151fTransientRetries;
static IMG_UINT32 gui32A151fInFlightFlips;
static IMG_BOOL gbA151fGateLogged;
static DEFINE_MUTEX(sA151fOpMutex);
static IMG_UINT64 gui64A151fUsage;

/*
 * L58: per-BM-context A151f ownership and admission state.  The list is
 * guarded by sA151fOpMutex; the flags/counters by sA151fCtxLock.  Entries are
 * created on first use and freed by the context-destroy callback after
 * eviction, so a reused BM_CONTEXT address can never inherit RELEASING.
 */
struct santos_a151f_ctx {
	IMG_HANDLE	hBMContext;
	IMG_BOOL	bRegistered;
	IMG_BOOL	bReleasing;
	IMG_UINT32	ui32Inflight;
	IMG_UINT32	ui32OwnedSlots;
	struct santos_a151f_ctx *psNext;
};

static struct santos_a151f_ctx *gpsA151fCtx;
static DEFINE_SPINLOCK(sA151fCtxLock);

static PVRSRV_ERROR santos_a151f_context_destroy_cb(IMG_HANDLE hBMContext,
						    IMG_VOID *pvPriv);

static struct santos_a151f_ctx *
santos_a151f_ctx_find(IMG_HANDLE hBMContext)
{
	struct santos_a151f_ctx *psCtx;

	for (psCtx = gpsA151fCtx; psCtx != IMG_NULL; psCtx = psCtx->psNext)
	{
		if (psCtx->hBMContext == hBMContext)
		{
			return psCtx;
		}
	}
	return IMG_NULL;
}

/* Caller holds sA151fOpMutex. */
static struct santos_a151f_ctx *
santos_a151f_ctx_get_or_create(IMG_HANDLE hBMContext)
{
	struct santos_a151f_ctx *psCtx;
	PVRSRV_ERROR eError;

	psCtx = santos_a151f_ctx_find(hBMContext);
	if (psCtx != IMG_NULL)
	{
		return psCtx;
	}

	psCtx = (struct santos_a151f_ctx *)AllocKernelMem(sizeof(*psCtx));
	if (!psCtx)
	{
		return IMG_NULL;
	}
	memset(psCtx, 0, sizeof(*psCtx));
	psCtx->hBMContext = hBMContext;

	eError = BM_RegisterContextDestroyCallback(hBMContext,
						   santos_a151f_context_destroy_cb,
						   hBMContext);
	if (eError != PVRSRV_OK)
	{
		FreeKernelMem(psCtx);
		return IMG_NULL;
	}
	psCtx->bRegistered = IMG_TRUE;
	psCtx->psNext = gpsA151fCtx;
	gpsA151fCtx = psCtx;

	return psCtx;
}

/* Admission critical section.  No sleeps under sA151fCtxLock. */
static IMG_BOOL santos_a151f_ctx_enter(struct santos_a151f_ctx *psCtx)
{
	unsigned long flags;
	IMG_BOOL bOk;

	spin_lock_irqsave(&sA151fCtxLock, flags);
	bOk = !psCtx->bReleasing;
	if (bOk)
	{
		psCtx->ui32Inflight++;
	}
	spin_unlock_irqrestore(&sA151fCtxLock, flags);
	return bOk;
}

static void santos_a151f_ctx_exit(struct santos_a151f_ctx *psCtx)
{
	unsigned long flags;

	spin_lock_irqsave(&sA151fCtxLock, flags);
	psCtx->ui32Inflight--;
	spin_unlock_irqrestore(&sA151fCtxLock, flags);
}

#define SANTOS_A151F_MAX_RETRIES 3

enum santos_a151f_copy_result {
	SANTOS_A151F_COPY_SUCCESS = 0,
	SANTOS_A151F_COPY_RETRY = 1,
	SANTOS_A151F_COPY_PERM_FAIL = 2,
};

static void santos_a151f_gate_fail(const char *pszWhy, IMG_UINT32 ui32Num,
				   IMG_SIZE_T uSize)
{
	if (!gbA151fGateLogged)
	{
		gbA151fGateLogged = IMG_TRUE;
		pr_info("santos-a151f: copy gated off (%s num=%u size=0x%lx)\n",
			pszWhy, ui32Num, (unsigned long)uSize);
	}
}

static IMG_BOOL santos_a151f_evict_slot_locked(IMG_UINT32 iSlot)
{
	struct santos_a151f_ctx *psCtx;
	PVRSRV_ERROR eError;

	if (iSlot >= SANTOS_A151F_MAX_SRC || !gasA151fSrc[iSlot].bInUse)
	{
		return IMG_TRUE;
	}

	if (gasA151fSrc[iSlot].pvMapWc)
	{
		vunmap(gasA151fSrc[iSlot].pvMapWc);
		gasA151fSrc[iSlot].pvMapWc = IMG_NULL;
	}

	if (gasA151fSrc[iSlot].psKMemInfo)
	{
		eError = FreeMemCallBackCommon(gasA151fSrc[iSlot].psKMemInfo, 0,
					       PVRSRV_FREE_CALLBACK_ORIGIN_IMPORTER);
		if (eError != PVRSRV_OK)
		{
			/* L58 fail closed: leave the slot so the context
			 * destruction preflight refuses and the backing is never
			 * released behind its retained owners. */
			return IMG_FALSE;
		}
		gasA151fSrc[iSlot].psKMemInfo = IMG_NULL;
	}

	psCtx = santos_a151f_ctx_find(gasA151fSrc[iSlot].hOwnerBMContext);
	if (psCtx != IMG_NULL && psCtx->ui32OwnedSlots > 0)
	{
		psCtx->ui32OwnedSlots--;
	}

	memset(&gasA151fSrc[iSlot], 0, sizeof(gasA151fSrc[iSlot]));
	return IMG_TRUE;
}

/* L58: context-destroy callback, registered per BM context. */
static PVRSRV_ERROR santos_a151f_context_destroy_cb(IMG_HANDLE hBMContext,
						    IMG_VOID *pvPriv)
{
	struct santos_a151f_ctx *psCtx;
	struct santos_a151f_ctx **ppsIter;
	unsigned long flags;
	IMG_UINT32 i;
	IMG_BOOL bFailed = IMG_FALSE;

	PVR_UNREFERENCED_PARAMETER(pvPriv);

	mutex_lock(&sA151fOpMutex);
	psCtx = santos_a151f_ctx_find(hBMContext);
	if (!psCtx)
	{
		mutex_unlock(&sA151fOpMutex);
		return PVRSRV_OK;
	}
	spin_lock_irqsave(&sA151fCtxLock, flags);
	psCtx->bReleasing = IMG_TRUE;
	spin_unlock_irqrestore(&sA151fCtxLock, flags);
	mutex_unlock(&sA151fOpMutex);

	/* Wait only for this context's admitted copies; never for other
	 * contexts and never while holding sA151fOpMutex. */
	for (;;)
	{
		IMG_UINT32 ui32Inflight;

		spin_lock_irqsave(&sA151fCtxLock, flags);
		ui32Inflight = psCtx->ui32Inflight;
		spin_unlock_irqrestore(&sA151fCtxLock, flags);
		if (ui32Inflight == 0)
		{
			break;
		}
		msleep(1);
	}

	mutex_lock(&sA151fOpMutex);
	for (i = 0; i < SANTOS_A151F_MAX_SRC; i++)
	{
		if (gasA151fSrc[i].bInUse &&
		    gasA151fSrc[i].hOwnerBMContext == hBMContext)
		{
			if (!santos_a151f_evict_slot_locked(i))
			{
				bFailed = IMG_TRUE;
			}
		}
	}
	if (bFailed)
	{
		mutex_unlock(&sA151fOpMutex);
		pr_err("santos-a151f: context teardown failed; context stays quarantined\n");
		return PVRSRV_ERROR_PROCESSING_BLOCKED;
	}
	for (ppsIter = &gpsA151fCtx; *ppsIter != IMG_NULL;
	     ppsIter = &(*ppsIter)->psNext)
	{
		if (*ppsIter == psCtx)
		{
			*ppsIter = psCtx->psNext;
			break;
		}
	}
	mutex_unlock(&sA151fOpMutex);
	FreeKernelMem(psCtx);
	return PVRSRV_OK;
}

static void santos_a151f_teardown_locked(IMG_BOOL bFullTeardown)
{
	IMG_UINT32 i;
	IMG_UINT32 ui32Mapped = 0;

	for (i = 0; i < SANTOS_A151F_MAX_SRC; i++)
	{
		if (gasA151fSrc[i].bInUse)
		{
			ui32Mapped++;
			(void)santos_a151f_evict_slot_locked(i);
		}
	}

	if (ui32Mapped)
	{
		pr_info("santos-a151f: %s teardown, unmapped %u source maps (total copies %u)\n",
			bFullTeardown ? "device" : "swapchain",
			ui32Mapped, gui32A151fCopies);
	}

	if (bFullTeardown)
	{
		/* L58: drop the per-context admission state; at DeInit no client
		 * contexts remain, so the registered callbacks can be removed
		 * before their entries are freed. */
		while (gpsA151fCtx)
		{
			struct santos_a151f_ctx *psCtx = gpsA151fCtx;

			gpsA151fCtx = psCtx->psNext;
			(void)BM_UnregisterContextDestroyCallback(psCtx->hBMContext,
								  santos_a151f_context_destroy_cb);
			FreeKernelMem(psCtx);
		}

		if (gpvA151fFb)
		{
			iounmap(gpvA151fFb);
			gpvA151fFb = IMG_NULL;
		}
	}
}

static void santos_a151f_quiesce_and_teardown(IMG_BOOL bFullTeardown)
{
	unsigned long flags;

	spin_lock_irqsave(&sA151fLock, flags);
	gbA151fShuttingDown = IMG_TRUE;
	spin_unlock_irqrestore(&sA151fLock, flags);

	/* Wait for all in-flight ProcessFlip callbacks to completely finish */
	while (1)
	{
		IMG_UINT32 in_flight;

		spin_lock_irqsave(&sA151fLock, flags);
		in_flight = gui32A151fInFlightFlips;
		spin_unlock_irqrestore(&sA151fLock, flags);
		if (in_flight == 0)
		{
			break;
		}
		msleep(1);
	}

	mutex_lock(&sA151fOpMutex);
	santos_a151f_teardown_locked(bFullTeardown);
	mutex_unlock(&sA151fOpMutex);
}

static void santos_a151f_teardown(void)
{
	santos_a151f_quiesce_and_teardown(IMG_TRUE);
}

/*
 * Cycle 7: bounded observation window used by DeInit preparation. No
 * destructive or retiring action is performed here; in-flight presentation
 * callbacks simply finish naturally. The admission freeze (queue.c) is what
 * stops new work from entering.
 */
#define SANTOS_A151F_QUIESCE_MS	500u

static IMG_UINT32 santos_a151f_in_flight(void)
{
	unsigned long flags;
	IMG_UINT32 in_flight;

	spin_lock_irqsave(&sA151fLock, flags);
	in_flight = gui32A151fInFlightFlips;
	spin_unlock_irqrestore(&sA151fLock, flags);
	return in_flight;
}

/*
 * Cycle 8 DC lifecycle state (owned by this file; see dc_nohw.h for the
 * public contract). The state is always mutated under sDCLifecycleMutex.
 *
 *  OFF     : no anchor, no Services registration, no module pin.
 *  RUNNING : anchor valid, Services DC node registered, cmdproc registered.
 *            One extra THIS_MODULE pin is held for the whole DC lifetime so
 *            that the module cannot be unloaded (and SysDeinitialise cannot
 *            start tearing Services down) while a cleanup could still fail
 *            and require a retry.
 *  STAGED  : the Services DC node has been removed (irreversible), so the
 *            remaining teardown must be resumed by a later Cleanup. The
 *            anchor is still valid and Re-init is refused until Cleanup
 *            reaches OFF; that is what keeps the freed device ID from being
 *            re-allocated while the old cmdproc table is still owned here.
 */
static int giDCNoHwLifecycle = DC_NOHW_LIFECYCLE_OFF;
static DEFINE_MUTEX(sDCLifecycleMutex);
static IMG_BOOL gbDCModulePinHeld = IMG_FALSE;

/*
 * Cycle 8: single lifecycle serialization. Both directions (enable via
 * DC_NOHW_Init and disable via DC_NOHW_Cleanup) run under this mutex, so
 * Init cannot observe or touch a staged/staging anchor and Deinit cannot
 * run concurrently with an enable. DC_NOHW_GetLifecycle only reads the
 * state under the same mutex so the /proc control surface never reports a
 * torn transition. Process context only.
 */


static struct santos_a151f_src *
santos_a151f_get_src(PPVRSRV_KERNEL_MEM_INFO psKMemInfo,
		     PDC_MEM_INFO psMemInfo,
		     IMG_HANDLE hOwnerBMContext,
		     IMG_BOOL *pbPermanentFail)
{
	LinuxMemArea *psArea;
	struct page **ppsPages;
	struct santos_a151f_src *psEntry;
	LINUX_PAT_RESULT ePatResult;
	void *pvMapWc = IMG_NULL;
	IMG_UINT32 i;
	int iSlot = -1;
	IMG_UINT64 ui64Oldest = (IMG_UINT64)-1LL;
	int iOldestSlot = -1;

	if (pbPermanentFail)
	{
		*pbPermanentFail = IMG_FALSE;
	}

	if (!psKMemInfo || !psMemInfo || !psKMemInfo->sMemBlk.hOSMemHandle ||
	    !psKMemInfo->sMemBlk.hBuffer)
	{
		return IMG_NULL;
	}
	if (!(psKMemInfo->ui32Flags & PVRSRV_HAP_WRITECOMBINE))
	{
		return IMG_NULL;
	}

	psArea = (LinuxMemArea *)psKMemInfo->sMemBlk.hOSMemHandle;
	if (psArea->eAreaType != LINUX_MEM_AREA_ALLOC_PAGES ||
	    psArea->uiByteSize != SANTOS_A151F_SIZE ||
	    !(psArea->ui32AreaFlags & PVRSRV_HAP_WRITECOMBINE))
	{
		return IMG_NULL;
	}

	ppsPages = psArea->uData.sPageList.ppsPageList;
	if (!ppsPages || !ppsPages[0])
	{
		return IMG_NULL;
	}
	for (i = 0; i < SANTOS_A151F_PAGES; i++)
	{
		if (!ppsPages[i])
		{
			return IMG_NULL;
		}
	}

	/* 1. Cache lookup: require full 6-field canonical identity.
	 * A cached slot keeps the meminfo references, so the backing area cannot
	 * be freed (and its PAT state cannot leave WC_TRACKED) while it is
	 * cached; cached entries need no re-transition. */
	for (i = 0; i < SANTOS_A151F_MAX_SRC; i++)
	{
		if (gasA151fSrc[i].bInUse &&
		    gasA151fSrc[i].psMemInfo == psMemInfo &&
		    gasA151fSrc[i].psKMemInfo == psKMemInfo &&
		    gasA151fSrc[i].psArea == psArea &&
		    gasA151fSrc[i].ppsPages == ppsPages &&
		    gasA151fSrc[i].pFirstPage == ppsPages[0] &&
		    gasA151fSrc[i].ui32DevVAddr == psKMemInfo->sDevVAddr.uiAddr)
		{
			gasA151fSrc[i].ui64LastUsed = ++gui64A151fUsage;
			return &gasA151fSrc[i];
		}
	}

	/* 2. Slot selection: find empty slot or LRU victim */
	for (i = 0; i < SANTOS_A151F_MAX_SRC; i++)
	{
		if (!gasA151fSrc[i].bInUse)
		{
			iSlot = (int)i;
			break;
		}
		if (gasA151fSrc[i].ui64LastUsed < ui64Oldest)
		{
			ui64Oldest = gasA151fSrc[i].ui64LastUsed;
			iOldestSlot = (int)i;
		}
	}

	if (iSlot < 0)
	{
		if (iOldestSlot >= 0)
		{
			if (!santos_a151f_evict_slot_locked(iOldestSlot))
			{
				return IMG_NULL;
			}
			iSlot = iOldestSlot;
		}
		else
		{
			return IMG_NULL;
		}
	}

	/* 3. Take Services references before mapping */
	PVRSRVKernelMemInfoIncRef(psKMemInfo);
	BM_Export(psKMemInfo->sMemBlk.hBuffer);

	/* 4. L56: the backing area must be PAT WC_TRACKED before A151f creates
	 * any CPU mapping or read.  The transition (and its mandatory recovery on
	 * failure) happens here, under the PAT mutex; no WC vmap may pre-exist. */
	ePatResult = LinuxMemAreaPatEnsureWC(psArea);
	if (ePatResult != LINUX_PAT_RESULT_OK)
	{
		if (pbPermanentFail &&
		    (ePatResult == LINUX_PAT_RESULT_QUARANTINED ||
		     ePatResult == LINUX_PAT_RESULT_UNSUPPORTED))
		{
			/* Fail closed: no retry can make an untracked/quarantined
			 * area safe for a CPU read. */
			*pbPermanentFail = IMG_TRUE;
		}
		FreeMemCallBackCommon(psKMemInfo, 0,
				       PVRSRV_FREE_CALLBACK_ORIGIN_IMPORTER);
		return IMG_NULL;
	}

	/* 5. Single WC mapping (the only A151f kernel alias) */
	pvMapWc = vmap(ppsPages, SANTOS_A151F_PAGES, VM_MAP,
		       pgprot_writecombine(PAGE_KERNEL));
	if (!pvMapWc)
	{
		/* Unwind Services references on mapping failure */
		FreeMemCallBackCommon(psKMemInfo, 0,
				       PVRSRV_FREE_CALLBACK_ORIGIN_IMPORTER);
		return IMG_NULL;
	}

	psEntry = &gasA151fSrc[iSlot];
	psEntry->psMemInfo = psMemInfo;
	psEntry->psKMemInfo = psKMemInfo;
	psEntry->psArea = psArea;
	psEntry->ppsPages = ppsPages;
	psEntry->pFirstPage = ppsPages[0];
	psEntry->ui32DevVAddr = psKMemInfo->sDevVAddr.uiAddr;
	psEntry->hOwnerBMContext = hOwnerBMContext;
	psEntry->pvMapWc = pvMapWc;
	psEntry->bInUse = IMG_TRUE;
	psEntry->ui64LastUsed = ++gui64A151fUsage;
	{
		struct santos_a151f_ctx *psCtx = santos_a151f_ctx_find(hOwnerBMContext);

		if (psCtx != IMG_NULL)
		{
			psCtx->ui32OwnedSlots++;
		}
	}

	pr_info("santos-a151f: src=%px wc-map=%px (PAT WC_TRACKED)\n",
		psMemInfo, pvMapWc);

	return psEntry;
}

static enum santos_a151f_copy_result
santos_a151f_try_copy(DC_NOHW_DEVINFO *psDevInfo,
		      DISPLAYCLASS_FLIP_COMMAND2 *psFlipCmd2)
{
	PDC_MEM_INFO psMemInfo;
	PPVRSRV_KERNEL_MEM_INFO psKMemInfo;
	struct santos_a151f_src *psSrc;
	IMG_SIZE_T uByteSize = 0;
	void *pvSrc;
	ktime_t tStart, tEnd;
	IMG_BOOL bPermanentFail = IMG_FALSE;
	IMG_BOOL bShuttingDown;
	IMG_HANDLE hCtx;
	struct santos_a151f_ctx *psCtx;
	unsigned long flags;

	/* 1. Fast early-bail check under spinlock */
	spin_lock_irqsave(&sA151fLock, flags);
	bShuttingDown = gbA151fShuttingDown;
	spin_unlock_irqrestore(&sA151fLock, flags);
	if (bShuttingDown)
	{
		return SANTOS_A151F_COPY_PERM_FAIL;
	}

	/* 2. Format & pointer validation - permanent failures */
	if (psFlipCmd2->ui32NumMemInfos != 1 || !psFlipCmd2->ppsMemInfos ||
	    !psFlipCmd2->ppsMemInfos[0])
	{
		santos_a151f_gate_fail("numMemInfos", psFlipCmd2->ui32NumMemInfos, 0);
		return SANTOS_A151F_COPY_PERM_FAIL;
	}

	psMemInfo = psFlipCmd2->ppsMemInfos[0];
	psKMemInfo = (PPVRSRV_KERNEL_MEM_INFO)psMemInfo;

	if (!psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetByteSize ||
	    psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetByteSize(psMemInfo,
								&uByteSize) != PVRSRV_OK ||
	    uByteSize != SANTOS_A151F_SIZE)
	{
		santos_a151f_gate_fail("byteSize", psFlipCmd2->ui32NumMemInfos,
				       uByteSize);
		return SANTOS_A151F_COPY_PERM_FAIL;
	}

	/* 3. Acquire operation mutex */
	mutex_lock(&sA151fOpMutex);

	/* Re-check shutdown under mutex */
	spin_lock_irqsave(&sA151fLock, flags);
	bShuttingDown = gbA151fShuttingDown;
	spin_unlock_irqrestore(&sA151fLock, flags);
	if (bShuttingDown)
	{
		mutex_unlock(&sA151fOpMutex);
		return SANTOS_A151F_COPY_PERM_FAIL;
	}

	/* L58: resolve the storage-owner BM context and admit under its
	 * per-context releasing/in-flight state before any slot commit. */
	hCtx = BM_GetContextHandleFromBuffer(psKMemInfo->sMemBlk.hBuffer);
	if (!hCtx)
	{
		santos_a151f_gate_fail("bmctx", psFlipCmd2->ui32NumMemInfos,
				       uByteSize);
		mutex_unlock(&sA151fOpMutex);
		return SANTOS_A151F_COPY_PERM_FAIL;
	}
	psCtx = santos_a151f_ctx_get_or_create(hCtx);
	if (!psCtx || !santos_a151f_ctx_enter(psCtx))
	{
		santos_a151f_gate_fail("releasing", psFlipCmd2->ui32NumMemInfos,
				       uByteSize);
		mutex_unlock(&sA151fOpMutex);
		return SANTOS_A151F_COPY_PERM_FAIL;
	}

	psSrc = santos_a151f_get_src(psKMemInfo, psMemInfo, hCtx, &bPermanentFail);
	if (!psSrc)
	{
		santos_a151f_gate_fail("area", psFlipCmd2->ui32NumMemInfos,
				       uByteSize);
		santos_a151f_ctx_exit(psCtx);
		mutex_unlock(&sA151fOpMutex);
		if (!bPermanentFail &&
		    gui32A151fTransientRetries < SANTOS_A151F_MAX_RETRIES)
		{
			gui32A151fTransientRetries++;
			return SANTOS_A151F_COPY_RETRY;
		}
		gui32A151fTransientRetries = 0;
		return SANTOS_A151F_COPY_PERM_FAIL;
	}

	if (!gpvA151fFb)
	{
		gpvA151fFb = ioremap_wc(SANTOS_A151F_FB_PHYS, SANTOS_A151F_SIZE);
		if (!gpvA151fFb)
		{
			santos_a151f_gate_fail("fbmap", psFlipCmd2->ui32NumMemInfos,
					       uByteSize);
			santos_a151f_ctx_exit(psCtx);
			mutex_unlock(&sA151fOpMutex);
			if (gui32A151fTransientRetries < SANTOS_A151F_MAX_RETRIES)
			{
				gui32A151fTransientRetries++;
				return SANTOS_A151F_COPY_RETRY;
			}
			gui32A151fTransientRetries = 0;
			return SANTOS_A151F_COPY_PERM_FAIL;
		}
	}

	if (!psSrc->pvMapWc)
	{
		santos_a151f_gate_fail("srcmap", psFlipCmd2->ui32NumMemInfos,
				       uByteSize);
		santos_a151f_ctx_exit(psCtx);
		mutex_unlock(&sA151fOpMutex);
		if (gui32A151fTransientRetries < SANTOS_A151F_MAX_RETRIES)
		{
			gui32A151fTransientRetries++;
			return SANTOS_A151F_COPY_RETRY;
		}
		gui32A151fTransientRetries = 0;
		return SANTOS_A151F_COPY_PERM_FAIL;
	}

	tStart = ktime_get();
	/*
	 * L56: the area is PAT WC_TRACKED before this vmap exists; the only CPU
	 * alias A151f uses is this WC mapping.  No WB alias and no per-frame
	 * flush are involved.
	 */
	memcpy_toio(gpvA151fFb, psSrc->pvMapWc, SANTOS_A151F_SIZE);
	wmb();
	tEnd = ktime_get();
	pvSrc = psSrc->pvMapWc;

	gui32A151fCopies++;
	gui32A151fTransientRetries = 0;

	if (gui32A151fCopies <= 5)
	{
		pr_info("santos-a151f[%u]: wc src=%px srcmap=%px dst=%px elapsed_us=%lld\n",
			gui32A151fCopies,
			psMemInfo, pvSrc, gpvA151fFb,
			(long long)ktime_to_us(ktime_sub(tEnd, tStart)));
	}
	else if ((gui32A151fCopies % 1000) == 0)
	{
		IMG_UINT32 ui32Cached = 0;
		IMG_UINT32 i;

		for (i = 0; i < SANTOS_A151F_MAX_SRC; i++)
		{
			if (gasA151fSrc[i].bInUse)
			{
				ui32Cached++;
			}
		}

		pr_info("santos-a151f[%u]: hb wc src=%px cached=%u elapsed_us=%lld\n",
			gui32A151fCopies,
			psMemInfo, ui32Cached,
			(long long)ktime_to_us(ktime_sub(tEnd, tStart)));
	}

	/* L58: admission exit on the success path. */
	santos_a151f_ctx_exit(psCtx);

	/* Release operation mutex */
	mutex_unlock(&sA151fOpMutex);

	return SANTOS_A151F_COPY_SUCCESS;
}

#if defined(SANTOS10_DC_NOHW_STANDALONE)
/* =========================================================================
 * Santos Read-Only VDC/GTT Retained-State Census Probe
 * ========================================================================= */
static int santos_gtt_census = 0;
module_param_named(santos_gtt_census, santos_gtt_census, int, 0444);
MODULE_PARM_DESC(santos_gtt_census,
	"Santos read-only VDC/GTT retained-state census probe (1=run once on init, 0=off)");

static bool gbSantosGttCensusDone = false;

static int santos_gtt_preflight = 0;
module_param_named(santos_gtt_preflight, santos_gtt_preflight, int, 0444);
MODULE_PARM_DESC(santos_gtt_preflight,
	"Santos VDC/GTT enable-transition preflight (1=run once on init, 0=off)");

static bool gbSantosGttPreflightDone = false;

extern struct pci_dev *gpsPVRLDMDev;

#define SANTOS_CENSUS_VDC_MAP_SIZE	0x80000u
#define SANTOS_CENSUS_PTE_COUNT		4096u
#define SANTOS_CENSUS_GTT_BYTES		(SANTOS_CENSUS_PTE_COUNT * sizeof(u32))
#define SANTOS_CENSUS_MAX_LOGGED_RUNS	128u

#define SANTOS_PCI_GMCH_CTRL		0x52
#define SANTOS_PCI_BSM			0x5c

/* VDC register offsets on BAR0 (Medfield/Cloverview golden values) */
#define SANTOS_VDC_PGETBL_CTL		0x02020u
#define SANTOS_VDC_DEV_READY		0x0b000u
#define SANTOS_VDC_DSI_FUNC		0x0b00cu
#define SANTOS_VDC_HTOTAL_A		0x60000u
#define SANTOS_VDC_VTOTAL_A		0x6000cu
#define SANTOS_VDC_MIPI			0x61190u
#define SANTOS_VDC_PP_STATUS		0x61200u
#define SANTOS_VDC_PP_CONTROL		0x61204u
#define SANTOS_VDC_PIPEACONF		0x70008u
#define SANTOS_VDC_PIPEASTAT		0x70024u
#define SANTOS_VDC_PIPEAFRAMEHIGH	0x70040u
#define SANTOS_VDC_PIPEAFRAMEPIXEL	0x70044u
#define SANTOS_VDC_DSPACNTR		0x70180u
#define SANTOS_VDC_DSPALINOFF		0x70184u
#define SANTOS_VDC_DSPASTRIDE		0x70188u
#define SANTOS_VDC_DSPAPOS		0x7018cu
#define SANTOS_VDC_DSPASIZE		0x70190u
#define SANTOS_VDC_DSPASURF		0x7019cu

static void santos_gtt_census_probe(void)
{
	struct pci_dev *pdev;
	bool bPutPdev = false;
	resource_size_t bar0_start, bar0_len;
	resource_size_t bar3_start, bar3_len;
	void __iomem *vdc_regs = NULL;
	void __iomem *gtt_map = NULL;
	u32 reg_pgetbl_ctl, reg_pipeaconf, reg_dspacntr, reg_dspastride;
	u32 reg_dspasize, reg_dspasurf, reg_mipi, reg_dev_ready;
	u32 reg_pipeastat, reg_dspalinoff, reg_dspapos, reg_dsi_func;
	u32 reg_pp_status, reg_pp_control;
	u32 gtt_enabled, gtt_phys_start;
	u32 pci_bsm = 0, stolen_base = 0, calc_stolen_size = 0;
	u16 pci_gmch_ctrl = 0;
	u32 dvmt_mode = 0, dvmt_mb = 0, gmch_enabled = 0;
	u32 *ptes = NULL;
	u32 i, run_start, total_runs;

	if (!santos_gtt_census || gbSantosGttCensusDone)
	{
		return;
	}

	gbSantosGttCensusDone = true;

	pr_info("SANTOS_GTT_CENSUS: === BEGIN VDC/GTT RETAINED-STATE CENSUS ===\n");

	/* 1. PCI / Resource Identity */
	pdev = gpsPVRLDMDev;
	if (!pdev)
	{
		pdev = pci_get_device(0x8086, 0x08c8, NULL);
		if (pdev)
		{
			bPutPdev = true;
		}
	}

	if (!pdev)
	{
		pr_err("SANTOS_GTT_CENSUS: ERROR: PCI device 8086:08c8 not found\n");
		pr_info("SANTOS_GTT_CENSUS: === END VDC/GTT RETAINED-STATE CENSUS (status=FAILED) ===\n");
		return;
	}

	bar0_start = pci_resource_start(pdev, 0);
	bar0_len   = pci_resource_len(pdev, 0);
	bar3_start = pci_resource_start(pdev, 3);
	bar3_len   = pci_resource_len(pdev, 3);

	pr_info("SANTOS_GTT_CENSUS: PCI BDF=%s dev=%04x:%04x\n",
		pci_name(pdev), pdev->vendor, pdev->device);
	pr_info("SANTOS_GTT_CENSUS:   BAR0: phys=0x%08llx..0x%08llx len=%llu (0x%llx)\n",
		(unsigned long long)bar0_start,
		(unsigned long long)(bar0_start + bar0_len - 1),
		(unsigned long long)bar0_len,
		(unsigned long long)bar0_len);
	pr_info("SANTOS_GTT_CENSUS:   BAR3: phys=0x%08llx..0x%08llx len=%llu (0x%llx) [resource ID only]\n",
		(unsigned long long)bar3_start,
		(unsigned long long)(bar3_start + bar3_len - 1),
		(unsigned long long)bar3_len,
		(unsigned long long)bar3_len);

	pci_read_config_dword(pdev, SANTOS_PCI_BSM, &pci_bsm);
	pci_read_config_word(pdev, SANTOS_PCI_GMCH_CTRL, &pci_gmch_ctrl);

	stolen_base = pci_bsm & PAGE_MASK;
	gmch_enabled = (pci_gmch_ctrl & 0x0004u) ? 1 : 0;
	dvmt_mode = (pci_gmch_ctrl >> 4) & 0x7u;
	dvmt_mb = (dvmt_mode == 1) ? 1 : (2 << (dvmt_mode - 1));

	pr_info("SANTOS_GTT_CENSUS: PCI CONFIG (read-only):\n");
	pr_info("SANTOS_GTT_CENSUS:   BSM       (0x5c) = 0x%08x -> stolen_base=0x%08x\n",
		pci_bsm, stolen_base);
	pr_info("SANTOS_GTT_CENSUS:   GMCH_CTRL (0x52) = 0x%04x (enabled=%u, dvmt_mode=%u -> %uMB)\n",
		pci_gmch_ctrl, gmch_enabled, dvmt_mode, dvmt_mb);

	/* 2. Retained VDC Register Snapshot (strictly read-only) */
	if (bar0_len < SANTOS_CENSUS_VDC_MAP_SIZE)
	{
		pr_err("SANTOS_GTT_CENSUS: ERROR: BAR0 len (%llu) < required 0x%x\n",
			(unsigned long long)bar0_len, SANTOS_CENSUS_VDC_MAP_SIZE);
		if (bPutPdev)
		{
			pci_dev_put(pdev);
		}
		pr_info("SANTOS_GTT_CENSUS: === END VDC/GTT RETAINED-STATE CENSUS (status=FAILED) ===\n");
		return;
	}

	vdc_regs = ioremap(bar0_start, SANTOS_CENSUS_VDC_MAP_SIZE);
	if (!vdc_regs)
	{
		pr_err("SANTOS_GTT_CENSUS: ERROR: failed to ioremap BAR0 (0x%08llx, size 0x%x)\n",
			(unsigned long long)bar0_start, SANTOS_CENSUS_VDC_MAP_SIZE);
		if (bPutPdev)
		{
			pci_dev_put(pdev);
		}
		pr_info("SANTOS_GTT_CENSUS: === END VDC/GTT RETAINED-STATE CENSUS (status=FAILED) ===\n");
		return;
	}

	reg_pgetbl_ctl = readl(vdc_regs + SANTOS_VDC_PGETBL_CTL);
	reg_dev_ready  = readl(vdc_regs + SANTOS_VDC_DEV_READY);
	reg_dsi_func   = readl(vdc_regs + SANTOS_VDC_DSI_FUNC);
	reg_mipi       = readl(vdc_regs + SANTOS_VDC_MIPI);
	reg_pp_status  = readl(vdc_regs + SANTOS_VDC_PP_STATUS);
	reg_pp_control = readl(vdc_regs + SANTOS_VDC_PP_CONTROL);
	reg_pipeaconf  = readl(vdc_regs + SANTOS_VDC_PIPEACONF);
	reg_pipeastat  = readl(vdc_regs + SANTOS_VDC_PIPEASTAT);
	reg_dspacntr   = readl(vdc_regs + SANTOS_VDC_DSPACNTR);
	reg_dspalinoff = readl(vdc_regs + SANTOS_VDC_DSPALINOFF);
	reg_dspastride = readl(vdc_regs + SANTOS_VDC_DSPASTRIDE);
	reg_dspapos    = readl(vdc_regs + SANTOS_VDC_DSPAPOS);
	reg_dspasize   = readl(vdc_regs + SANTOS_VDC_DSPASIZE);
	reg_dspasurf   = readl(vdc_regs + SANTOS_VDC_DSPASURF);

	pr_info("SANTOS_GTT_CENSUS: VDC REG SNAPSHOT (read-only):\n");
	pr_info("SANTOS_GTT_CENSUS:   PSB_PGETBL_CTL (+0x02020) = 0x%08x\n", reg_pgetbl_ctl);
	pr_info("SANTOS_GTT_CENSUS:   DEV_READY      (+0x0b000) = 0x%08x (ready=%u)\n",
		reg_dev_ready, reg_dev_ready & 0x1);
	pr_info("SANTOS_GTT_CENSUS:   DSI_FUNC       (+0x0b00c) = 0x%08x\n", reg_dsi_func);
	pr_info("SANTOS_GTT_CENSUS:   MIPI           (+0x61190) = 0x%08x (port_enable=%u)\n",
		reg_mipi, (reg_mipi >> 31) & 0x1);
	pr_info("SANTOS_GTT_CENSUS:   PP_STATUS      (+0x61200) = 0x%08x\n", reg_pp_status);
	pr_info("SANTOS_GTT_CENSUS:   PP_CONTROL     (+0x61204) = 0x%08x (pwr_on=%u)\n",
		reg_pp_control, (reg_pp_control >> 31) & 0x1);
	pr_info("SANTOS_GTT_CENSUS:   PIPEACONF      (+0x70008) = 0x%08x (pipe_enable=%u)\n",
		reg_pipeaconf, (reg_pipeaconf >> 31) & 0x1);
	pr_info("SANTOS_GTT_CENSUS:   PIPEASTAT      (+0x70024) = 0x%08x\n", reg_pipeastat);
	pr_info("SANTOS_GTT_CENSUS:   DSPACNTR       (+0x70180) = 0x%08x (plane_enable=%u)\n",
		reg_dspacntr, (reg_dspacntr >> 31) & 0x1);
	pr_info("SANTOS_GTT_CENSUS:   DSPALINOFF     (+0x70184) = 0x%08x\n", reg_dspalinoff);
	pr_info("SANTOS_GTT_CENSUS:   DSPASTRIDE     (+0x70188) = 0x%08x (stride=%u bytes)\n",
		reg_dspastride, reg_dspastride);
	pr_info("SANTOS_GTT_CENSUS:   DSPAPOS        (+0x7018c) = 0x%08x\n", reg_dspapos);
	pr_info("SANTOS_GTT_CENSUS:   DSPASIZE       (+0x70190) = 0x%08x (w=%u, h=%u)\n",
		reg_dspasize,
		(reg_dspasize & 0xfffu) + 1,
		((reg_dspasize >> 16) & 0xfffu) + 1);
	pr_info("SANTOS_GTT_CENSUS:   DSPASURF       (+0x7019c) = 0x%08x\n", reg_dspasurf);

	/* 3. PSB_PGETBL_CTL Decode & Memory Relationships */
	gtt_enabled    = (reg_pgetbl_ctl & 0x00000001u);
	gtt_phys_start = (reg_pgetbl_ctl & PAGE_MASK);
	calc_stolen_size = (gtt_phys_start > stolen_base) ? (gtt_phys_start - stolen_base) : 0;

	pr_info("SANTOS_GTT_CENSUS: PSB_PGETBL_CTL DECODE:\n");
	pr_info("SANTOS_GTT_CENSUS:   raw=0x%08x enabled_bit=%u gtt_phys_start=0x%08x\n",
		reg_pgetbl_ctl, gtt_enabled, gtt_phys_start);
	pr_info("SANTOS_GTT_CENSUS: MEMORY RELATIONSHIPS (derived):\n");
	pr_info("SANTOS_GTT_CENSUS:   stolen_base=0x%08x gtt_phys_start=0x%08x stolen_span=0x%08x..0x%08x (size=%u KiB, %u MiB)\n",
		stolen_base, gtt_phys_start, stolen_base,
		gtt_phys_start ? (gtt_phys_start - 1) : 0,
		calc_stolen_size >> 10, calc_stolen_size >> 20);
	pr_info("SANTOS_GTT_CENSUS:   retained_fb_phys=0x3f000000 (delta from stolen_base: 0x%x bytes)\n",
		(stolen_base <= 0x3f000000u) ? (0x3f000000u - stolen_base) : 0xffffffffu);

	if (gtt_phys_start == 0 || (gtt_phys_start & ~PAGE_MASK) != 0 ||
	    gtt_phys_start < 0x01000000u)
	{
		pr_err("SANTOS_GTT_CENSUS: ERROR: invalid gtt_phys_start=0x%08x (zero, unaligned, or below 16MB)\n",
			gtt_phys_start);
		iounmap(vdc_regs);
		if (bPutPdev)
		{
			pci_dev_put(pdev);
		}
		pr_info("SANTOS_GTT_CENSUS: === END VDC/GTT RETAINED-STATE CENSUS (status=FAILED) ===\n");
		return;
	}

	if (!gtt_enabled)
	{
		pr_info("SANTOS_GTT_CENSUS: GTT hardware translation currently disabled;\n");
		pr_info("SANTOS_GTT_CENSUS: backing-table census performed read-only.\n");
	}

	/* 4. Real GTT Backing Census (first 4096 PTEs / 16 KiB, strictly read-only) */
	/*
	 * In modern Linux 7.2 on x86, ioremap() defaults to uncached (UC-) memory,
	 * providing coherent uncached read access to hardware-managed GTT page tables
	 * in reserved physical RAM. ioremap_nocache() from 3.4 is obsolete and removed.
	 */
	gtt_map = ioremap(gtt_phys_start, SANTOS_CENSUS_GTT_BYTES);
	if (!gtt_map)
	{
		pr_err("SANTOS_GTT_CENSUS: ERROR: failed to ioremap GTT at 0x%08x (size %u bytes)\n",
			gtt_phys_start, (unsigned int)SANTOS_CENSUS_GTT_BYTES);
		iounmap(vdc_regs);
		if (bPutPdev)
		{
			pci_dev_put(pdev);
		}
		pr_info("SANTOS_GTT_CENSUS: === END VDC/GTT RETAINED-STATE CENSUS (status=FAILED) ===\n");
		return;
	}

	ptes = kmalloc(SANTOS_CENSUS_GTT_BYTES, GFP_KERNEL);
	if (!ptes)
	{
		pr_err("SANTOS_GTT_CENSUS: ERROR: failed to allocate memory for PTE buffer\n");
		iounmap(gtt_map);
		iounmap(vdc_regs);
		if (bPutPdev)
		{
			pci_dev_put(pdev);
		}
		pr_info("SANTOS_GTT_CENSUS: === END VDC/GTT RETAINED-STATE CENSUS (status=FAILED) ===\n");
		return;
	}

	for (i = 0; i < SANTOS_CENSUS_PTE_COUNT; i++)
	{
		ptes[i] = readl(gtt_map + (i * sizeof(u32)));
	}

	/* Immediately unmap GTT mapping after reading */
	iounmap(gtt_map);
	gtt_map = NULL;

	/* 5. Compact Topology Output (bounded to SANTOS_CENSUS_MAX_LOGGED_RUNS) */
	pr_info("SANTOS_GTT_CENSUS: GTT TOPOLOGY (PTE 0..%u, count=%u, size=16 KiB):\n",
		SANTOS_CENSUS_PTE_COUNT - 1, SANTOS_CENSUS_PTE_COUNT);

	run_start  = 0;
	total_runs = 0;

	while (run_start < SANTOS_CENSUS_PTE_COUNT)
	{
		u32 run_end = run_start;
		u32 pte0 = ptes[run_start];
		u32 pfn0 = pte0 >> PAGE_SHIFT;
		u32 flags0 = pte0 & 0xfffu;
		int run_type = 0; /* 0=single, 1=constant, 2=linear */

		if (run_start + 1 < SANTOS_CENSUS_PTE_COUNT)
		{
			u32 pte1 = ptes[run_start + 1];
			u32 pfn1 = pte1 >> PAGE_SHIFT;
			u32 flags1 = pte1 & 0xfffu;

			if (pte1 == pte0)
			{
				run_type = 1;
			}
			else if (flags1 == flags0 && pfn1 == pfn0 + 1)
			{
				run_type = 2;
			}
		}

		if (run_type == 1)
		{
			while (run_end + 1 < SANTOS_CENSUS_PTE_COUNT &&
			       ptes[run_end + 1] == pte0)
			{
				run_end++;
			}
		}
		else if (run_type == 2)
		{
			while (run_end + 1 < SANTOS_CENSUS_PTE_COUNT)
			{
				u32 next_pte = ptes[run_end + 1];
				u32 next_pfn = next_pte >> PAGE_SHIFT;
				u32 next_flags = next_pte & 0xfffu;
				u32 expected_pfn = pfn0 + (run_end + 1 - run_start);

				if (next_flags == flags0 && next_pfn == expected_pfn)
				{
					run_end++;
				}
				else
				{
					break;
				}
			}
		}

		if (total_runs < SANTOS_CENSUS_MAX_LOGGED_RUNS)
		{
			u32 count = run_end - run_start + 1;
			u32 pte_last = ptes[run_end];
			const char *desc;

			if (run_type == 1)
			{
				if (pte0 == 0)
				{
					desc = "constant_zero (unmapped)";
				}
				else if (flags0 & 0x1)
				{
					desc = "constant_valid (scratch/const candidate)";
				}
				else
				{
					desc = "constant_invalid";
				}
			}
			else if (run_type == 2)
			{
				desc = "linear_pfn_progression (+4K/page)";
			}
			else
			{
				if (pte0 == 0)
				{
					desc = "single_entry (zero)";
				}
				else if (flags0 & 0x1)
				{
					desc = "single_entry (valid)";
				}
				else
				{
					desc = "single_entry (invalid)";
				}
			}

			pr_info("SANTOS_GTT_CENSUS:   run[%2u]: [%4u..%4u] count=%4u first=0x%08x last=0x%08x flags=0x%03x (%s)\n",
				total_runs, run_start, run_end, count,
				pte0, pte_last, flags0, desc);
		}

		total_runs++;
		run_start = run_end + 1;
	}

	if (total_runs > SANTOS_CENSUS_MAX_LOGGED_RUNS)
	{
		pr_info("SANTOS_GTT_CENSUS:   ... %u additional topology runs suppressed (total_runs=%u)\n",
			total_runs - SANTOS_CENSUS_MAX_LOGGED_RUNS, total_runs);
	}

	/* 6. Critical Samples (0, 1, 998, 999, 1000, 1023, 1024, 2023, 2048, 3047, 3072, 4095) */
	pr_info("SANTOS_GTT_CENSUS: CRITICAL SAMPLES:\n");
	pr_info("SANTOS_GTT_CENSUS:   PTE[0]=0x%08x PTE[1]=0x%08x PTE[998]=0x%08x PTE[999]=0x%08x\n",
		ptes[0], ptes[1], ptes[998], ptes[999]);
	pr_info("SANTOS_GTT_CENSUS:   PTE[1000]=0x%08x PTE[1023]=0x%08x PTE[1024]=0x%08x PTE[2023]=0x%08x\n",
		ptes[1000], ptes[1023], ptes[1024], ptes[2023]);
	pr_info("SANTOS_GTT_CENSUS:   PTE[2048]=0x%08x PTE[3047]=0x%08x PTE[3072]=0x%08x PTE[4095]=0x%08x\n",
		ptes[2048], ptes[3047], ptes[3072], ptes[4095]);

	kfree(ptes);
	iounmap(vdc_regs);
	if (bPutPdev)
	{
		pci_dev_put(pdev);
	}

	pr_info("SANTOS_GTT_CENSUS: === END VDC/GTT RETAINED-STATE CENSUS (status=SUCCESS) ===\n");
}

/*
 * Frame Counter Helpers (Pipe A):
 * PIPEAFRAMEHIGH (0x70040) + PIPEAFRAMEPIXEL (0x70044) -> 24-bit frame counter.
 */
static inline u32 santos_cand3b_read_frame_counter(void __iomem *pvVdcRegs)
{
	u32 high1, high2, low;
	do {
		high1 = ioread32(pvVdcRegs + SANTOS_VDC_PIPEAFRAMEHIGH);
		low   = ioread32(pvVdcRegs + SANTOS_VDC_PIPEAFRAMEPIXEL);
		high2 = ioread32(pvVdcRegs + SANTOS_VDC_PIPEAFRAMEHIGH);
	} while (high1 != high2);

	return ((high1 << 8) | (low >> 24)) & 0x00ffffffu;
}

/*
 * Wrap-safe 24-bit frame counter comparison:
 * Returns true if u32Curr is greater than or equal to u32Target in 24-bit modular arithmetic.
 *
 * Calculation:
 * The difference (u32Curr - u32Target) is shifted left by 8 bits into a signed 32-bit integer,
 * then arithmetic right shifted by 8 bits to sign-extend bit 23 into bits [31:24].
 * A signed result >= 0 indicates that u32Curr is ahead of or at u32Target.
 *
 * Valid comparison window:
 * Exactly half the 24-bit range: [-2^23, 2^23 - 1] frames = [-8,388,608 .. +8,388,607] frames.
 * At 60.14 Hz (~16.63 ms/frame), this represents a valid comparison window of:
 * 8,388,608 frames / 60.14 Hz = 139,484 seconds ≈ 38.74 hours.
 */
static inline bool santos_cand3b_frame_passed(u32 u32Curr, u32 u32Target)
{
	s32 diff = ((s32)((u32Curr - u32Target) << 8)) >> 8;
	return diff >= 0;
}

/* =========================================================================
 * Santos VDC/GTT Enable-Transition Hardware Preflight Probe
 *
 * Narrowly scoped preflight test:
 * Tests disabled -> enabled -> disabled GTT/GMCH transition on retained state:
 * - DSPASURF remains 0 (baseline stolen FB at 0x3f000000)
 * - PTE[0] = 0x3f000001 (S-Boot stolen FB identity mapping to 0x3f000000)
 * - Zero dynamic PTE writes, zero SGX mapping, zero DSPASURF alteration.
 * - Verifies frame-counter progression during 100ms enabled window.
 * - Restores GMCH bit 2 and PGETBL bit 0 cleanly to disabled state.
 * - Note: GMCH bit 2 and PGETBL bit 0 are non-reflective (read-as-zero) on this HW,
 *   as proven by p10 golden Linux 3.4 runtime census (active GTT scanout with bits=0).
 * ========================================================================= */
static void santos_gtt_preflight_probe(void)
{
	struct pci_dev *pdev;
	bool bPutPdev = false;
	resource_size_t bar0_start, bar0_len;
	void __iomem *vdc_regs = NULL;
	void __iomem *gtt_map = NULL;
	u32 reg_pgetbl_ctl, reg_pipeaconf, reg_dspacntr, reg_dspasurf;
	u32 pci_bsm = 0, stolen_base = 0;
	u16 pci_gmch_ctrl = 0, gmch_during = 0, gmch_post = 0;
	u32 pte0 = 0, pgetbl_during = 0, pgetbl_post = 0;
	u32 f0, f1, f_enable, f_during, f_restore, f_post;
	bool bSuccess = false;

	if (!santos_gtt_preflight || gbSantosGttPreflightDone)
	{
		return;
	}

	gbSantosGttPreflightDone = true;

	pr_info("SANTOS_GTT_PREFLIGHT: === BEGIN VDC/GTT ENABLE-TRANSITION PREFLIGHT ===\n");

	/* 1. Locate PCI device */
	pdev = gpsPVRLDMDev;
	if (!pdev)
	{
		pdev = pci_get_device(0x8086, 0x08c8, NULL);
		if (pdev)
		{
			bPutPdev = true;
		}
	}

	if (!pdev)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: ERROR: PCI device 8086:08c8 not found\n");
		pr_info("SANTOS_GTT_PREFLIGHT: === END VDC/GTT ENABLE-TRANSITION PREFLIGHT (status=FAILED) ===\n");
		return;
	}

	bar0_start = pci_resource_start(pdev, 0);
	bar0_len   = pci_resource_len(pdev, 0);

	if (bar0_len < SANTOS_CENSUS_VDC_MAP_SIZE)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: ERROR: BAR0 len (%llu) < required 0x%x\n",
			(unsigned long long)bar0_len, SANTOS_CENSUS_VDC_MAP_SIZE);
		if (bPutPdev)
			pci_dev_put(pdev);
		pr_info("SANTOS_GTT_PREFLIGHT: === END VDC/GTT ENABLE-TRANSITION PREFLIGHT (status=FAILED) ===\n");
		return;
	}

	vdc_regs = ioremap(bar0_start, SANTOS_CENSUS_VDC_MAP_SIZE);
	if (!vdc_regs)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: ERROR: failed to ioremap BAR0\n");
		if (bPutPdev)
			pci_dev_put(pdev);
		pr_info("SANTOS_GTT_PREFLIGHT: === END VDC/GTT ENABLE-TRANSITION PREFLIGHT (status=FAILED) ===\n");
		return;
	}

	/* 2. Read baseline PCI & VDC registers */
	pci_read_config_dword(pdev, SANTOS_PCI_BSM, &pci_bsm);
	pci_read_config_word(pdev, SANTOS_PCI_GMCH_CTRL, &pci_gmch_ctrl);
	stolen_base = pci_bsm & PAGE_MASK;

	reg_pgetbl_ctl = readl(vdc_regs + SANTOS_VDC_PGETBL_CTL);
	reg_dspasurf   = readl(vdc_regs + SANTOS_VDC_DSPASURF);
	reg_pipeaconf  = readl(vdc_regs + SANTOS_VDC_PIPEACONF);
	reg_dspacntr   = readl(vdc_regs + SANTOS_VDC_DSPACNTR);

	pr_info("SANTOS_GTT_PREFLIGHT: Precondition check:\n");
	pr_info("SANTOS_GTT_PREFLIGHT:   BSM: 0x%08x (stolen_base=0x%08x)\n", pci_bsm, stolen_base);
	pr_info("SANTOS_GTT_PREFLIGHT:   GMCH_CTRL: 0x%04x (bit 2 = %u)\n",
		pci_gmch_ctrl, (pci_gmch_ctrl & 0x0004u) ? 1 : 0);
	pr_info("SANTOS_GTT_PREFLIGHT:   PGETBL_CTL: 0x%08x (bit 0 = %u, base=0x%08x)\n",
		reg_pgetbl_ctl, reg_pgetbl_ctl & 1, reg_pgetbl_ctl & PAGE_MASK);
	pr_info("SANTOS_GTT_PREFLIGHT:   DSPASURF: 0x%08x\n", reg_dspasurf);
	pr_info("SANTOS_GTT_PREFLIGHT:   PIPEACONF: 0x%08x (pipe_en=%u), DSPACNTR: 0x%08x (plane_en=%u)\n",
		reg_pipeaconf, (reg_pipeaconf & BIT(31)) ? 1 : 0,
		reg_dspacntr, (reg_dspacntr & BIT(31)) ? 1 : 0);

	/* Precondition assertions */
	if (stolen_base != 0x3f000000u)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: stolen_base 0x%08x != expected 0x3f000000\n", stolen_base);
		goto exit_unmap_vdc;
	}
	if ((pci_gmch_ctrl & 0x0004u) != 0)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: GMCH bit 2 already set (0x%04x)\n", pci_gmch_ctrl);
		goto exit_unmap_vdc;
	}
	if ((reg_pgetbl_ctl & 0x00000001u) != 0)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: PGETBL bit 0 already set (0x%08x)\n", reg_pgetbl_ctl);
		goto exit_unmap_vdc;
	}
	if ((reg_pgetbl_ctl & PAGE_MASK) != 0x3ffc0000u)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: PGETBL base 0x%08x != expected 0x3ffc0000\n",
		       reg_pgetbl_ctl & PAGE_MASK);
		goto exit_unmap_vdc;
	}
	if (reg_dspasurf != 0x00000000u)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: DSPASURF 0x%08x != 0\n", reg_dspasurf);
		goto exit_unmap_vdc;
	}
	if (!(reg_pipeaconf & BIT(31)) || !(reg_dspacntr & BIT(31)))
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: Pipe A or Plane A is disabled\n");
		goto exit_unmap_vdc;
	}

	/* Map GTT entry 0 */
	gtt_map = ioremap(0x3ffc0000u, sizeof(u32));
	if (!gtt_map)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: ERROR: failed to ioremap GTT PTE[0] at 0x3ffc0000\n");
		goto exit_unmap_vdc;
	}
	pte0 = readl(gtt_map);
	pr_info("SANTOS_GTT_PREFLIGHT:   PTE[0] at 0x3ffc0000 = 0x%08x\n", pte0);
	if (pte0 != 0x3f000001u)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: PTE[0] 0x%08x != expected identity 0x3f000001\n", pte0);
		goto exit_unmap_gtt;
	}

	/* 3. Liveness check before enable */
	f0 = santos_cand3b_read_frame_counter(vdc_regs);
	msleep(35);
	f1 = santos_cand3b_read_frame_counter(vdc_regs);
	pr_info("SANTOS_GTT_PREFLIGHT: Pre-enable frame counter: f0=%u -> f1=%u (delta=%d)\n",
		f0, f1, (int)((f1 - f0) & 0x00ffffffu));
	if (f0 == f1)
	{
		pr_err("SANTOS_GTT_PREFLIGHT: PRECONDITION FAIL: frame counter is stalled before test\n");
		goto exit_unmap_gtt;
	}

	/* 4. ENABLE transition */
	pr_info("SANTOS_GTT_PREFLIGHT: Step 1: Enabling GMCH bit 2 (0x4) and PGETBL bit 0 (0x1) on retained identity PTE[0]...\n");
	pci_write_config_word(pdev, SANTOS_PCI_GMCH_CTRL, pci_gmch_ctrl | 0x0004u);
	writel(reg_pgetbl_ctl | 0x00000001u, vdc_regs + SANTOS_VDC_PGETBL_CTL);
	(void)readl(vdc_regs + SANTOS_VDC_PGETBL_CTL);
	f_enable = santos_cand3b_read_frame_counter(vdc_regs);

	/* 5. Observe 100 ms window during ENABLED state */
	msleep(100);
	f_during = santos_cand3b_read_frame_counter(vdc_regs);
	pci_read_config_word(pdev, SANTOS_PCI_GMCH_CTRL, &gmch_during);
	pgetbl_during = readl(vdc_regs + SANTOS_VDC_PGETBL_CTL);

	pr_info("SANTOS_GTT_PREFLIGHT: Enabled window (100ms): f_enable=%u -> f_during=%u (delta=%d), GMCH=0x%04x PGETBL=0x%08x\n",
		f_enable, f_during, (int)((f_during - f_enable) & 0x00ffffffu),
		gmch_during, pgetbl_during);

	/* 6. RESTORE transition */
	pr_info("SANTOS_GTT_PREFLIGHT: Step 2: Restoring GMCH and PGETBL to original disabled state...\n");
	pci_write_config_word(pdev, SANTOS_PCI_GMCH_CTRL, pci_gmch_ctrl);
	writel(reg_pgetbl_ctl, vdc_regs + SANTOS_VDC_PGETBL_CTL);
	(void)readl(vdc_regs + SANTOS_VDC_PGETBL_CTL);
	f_restore = santos_cand3b_read_frame_counter(vdc_regs);

	/* 7. Observe 50 ms window during RESTORED state */
	msleep(50);
	f_post = santos_cand3b_read_frame_counter(vdc_regs);
	pci_read_config_word(pdev, SANTOS_PCI_GMCH_CTRL, &gmch_post);
	pgetbl_post = readl(vdc_regs + SANTOS_VDC_PGETBL_CTL);

	pr_info("SANTOS_GTT_PREFLIGHT: Restored window (50ms): f_restore=%u -> f_post=%u (delta=%d), GMCH=0x%04x PGETBL=0x%08x\n",
		f_restore, f_post, (int)((f_post - f_restore) & 0x00ffffffu),
		gmch_post, pgetbl_post);

	/* 8. Verify all assertions:
	 * Note: GMCH_CTRL bit 2 and PGETBL_CTL bit 0 are proven non-reflective / read-as-zero
	 * on this hardware (verified on live golden Linux 3.4 runtime census; active GTT scanout
	 * operates with both bits reading 0).
	 * Readback is performed solely for posted-write/drain ordering (matching golden driver).
	 * Success is verified by continuous, non-stalled hardware frame-counter progression
	 * across both the enabled observation window (100 ms) and restored observation window (50 ms).
	 */
	if (santos_cand3b_frame_passed(f_during, (f_enable + 2) & 0x00ffffffu) &&
	    santos_cand3b_frame_passed(f_post, (f_restore + 1) & 0x00ffffffu))
	{
		bSuccess = true;
	}

exit_unmap_gtt:
	if (gtt_map)
		iounmap(gtt_map);
exit_unmap_vdc:
	if (vdc_regs)
		iounmap(vdc_regs);
	if (bPutPdev)
		pci_dev_put(pdev);

	if (bSuccess)
	{
		pr_info("SANTOS_GTT_PREFLIGHT: === END VDC/GTT ENABLE-TRANSITION PREFLIGHT (status=PASS) ===\n");
	}
	else
	{
		pr_err("SANTOS_GTT_PREFLIGHT: === END VDC/GTT ENABLE-TRANSITION PREFLIGHT (status=FAIL) ===\n");
	}
}

/* =========================================================================
 * Santos Candidate 3B Zero-Copy Display Scanout Path
 *
 * Golden Authority & References:
 * - GMCH & PGETBL Enable: src/santos10-3.4/drivers/staging/intel_media/common/psb_gtt.c:90-97
 *   * _PSB_GMCH_ENABLED = 0x4 (bit 2 of PCI 0x52) in psb_drv.h:134
 *   * _PSB_PGETBL_ENABLED = 0x00000001 (bit 0 of VDC 0x2020) in psb_drv.h:136
 *   * Posting reads: (void)PSB_RVDC32(PSB_PGETBL_CTL)
 * - Dynamic GTT Region: psb_gtt.c:394-403
 *   * tt_start = 4031 (first dynamic slot, aperture offset 0x00FBF000)
 *   * PTE encoding: (pfn << PAGE_SHIFT) | 0x1 (psb_gtt_mask_pte(pfn, 0))
 *   * Posting write: (void)ioread32(cur_page - 1)
 *   * Restoring unmapped/released slots: scratch page PTE (dev_priv->scratch_page)
 *   * Hardware flush/invalidate: No TLB invalidate register exists in golden source.
 * - Presentation & Retirement (r8 contract; r7 remains the device-verified revision):
 *   * DSPASURF programming: REG_WRITE(dspsurf, Start); REG_READ(dspsurf); (psb_intel_display.c:1107-1115)
 *   * Frame counter read: psb_get_vblank_counter double-read (mdfld_output.c:26-44)
 *   * Golden 3.4 retirement: ProcessFlip2() programs the successor and marks it bFlipped;
 *     MRSTLFBVSyncIHandler() completes the PREVIOUS sLastItem cookie on the first following
 *     vblank (drmlfb_displayclass.c:129-150, 1548-1626).  The current conservative
 *     frame-counter analogue is target = F_post + 1 (first boundary after the successor
 *     DSPASURF write).  This is a conservative safety boundary, NOT a proven minimum: a
 *     vblank slipping between the posted DSPASURF write and the coherent F_post sample can
 *     make it wait one extra boundary.  r8 keeps it unchanged and makes no minimum claim.
 *   * Async Services completion (context-scoped): PVRSRVCommandCompleteKM performs the
 *     whole completion regardless of bScheduleMISR; only bScheduleMISR=true additionally
 *     calls OSScheduleMISR (queue.c:1402-1514).  The retirement worker (asynchronous
 *     workqueue context) is the ONLY Candidate 3B site that passes IMG_TRUE, so queued
 *     display work continues (golden vblank analogue MRST_TRUE/IMG_TRUE,
 *     drmlfb_displayclass.c:140, 1576, 1868-2132).  Every teardown context - external
 *     DestroyDCSwapChain/Deinit as well as try_flip()/ProcessFlip internal fail-closed -
 *     passes IMG_FALSE: a terminal teardown must not request another Services
 *     queue-processing pass while swapchain/command-processor structures are being freed.
 *     try_flip() inline retire/drain completions and the A151f fallback also pass
 *     IMG_FALSE.
 *   * Prompt observation: a dedicated ordered workqueue runs a bounded frame-counter poll
 *     (1-2 ms sleeps, no lock held across sleep) so a two-buffer 60 Hz chain is not held for an
 *     extra frame.  Completion still requires observing F_curr >= target; on a stalled counter
 *     the pending cookies are preserved (never completed early).
 *   * Lifecycle serialization (r8/r8r1): a single operation mutex serializes the whole
 *     try_flip (buffer mapping + DSPASURF commit) against teardown (baseline latch + GTT
 *     wipe/unmap).  teardown publishes bShuttingDown under sLock first and then acquires the
 *     gate; try_flip re-checks bShuttingDown || bObserverStalled || bFailed under sLock
 *     immediately before the DSPASURF commit (final admission check), so a racing teardown
 *     cannot wipe/unmap the GTT while a flip can still map PTEs or program DSPASURF.  A
 *     queued or re-entered retirement worker exits promptly once bShuttingDown/
 *     bObserverStalled is observed.  sLock is statically initialized, so external teardown
 *     is safe before the first init and after an early init failure; init allocates the
 *     retirement workqueue before publishing any dynamic scratch PTE, and the teardown
 *     unwind clears the retained dynamic PTEs before freeing the scratch page.
 *   * Fallback: DSPASURF = 0, bounded wait for latching, restore GMCH/PGETBL.
 * ========================================================================= */

#ifndef _PSB_GMCH_ENABLED
#define _PSB_GMCH_ENABLED	0x4
#endif
#ifndef _PSB_PGETBL_ENABLED
#define _PSB_PGETBL_ENABLED	0x00000001u
#endif

#define SANTOS_CAND3B_MAX_BUFFERS	8u
#define SANTOS_CAND3B_PAGES_PER_BUF	1000u
#define SANTOS_CAND3B_GTT_BASE_PTE	4031u
#define SANTOS_CAND3B_GTT_POOL_END_PTE	(SANTOS_CAND3B_GTT_BASE_PTE + SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF)
#define SANTOS_CAND3B_RETIRE_QUEUE_SIZE	8u

/* r7 prompt boundary observation: dedicated ordered workqueue + bounded frame-counter poll.
 * Poll cadence 1-2 ms (hrtimer-backed usleep_range, CONFIG_HZ=1000); strict bound below.
 * No lock is held across the sleep; on stall the pending cookies are preserved. */
#define SANTOS_CAND3B_RETIRE_POLL_US_MIN	1000u
#define SANTOS_CAND3B_RETIRE_POLL_US_MAX	2000u
#define SANTOS_CAND3B_RETIRE_MAX_POLLS		100u

/*
 * Note on runtime-switchability:
 * santos_cand3b is strictly non-runtime-switchable (0444, read-only in sysfs).
 * Toggling scanout architecture mid-flight via sysfs while compositors present
 * active frames introduces dangerous race conditions and state desynchronization
 * (A151f copying to stolen FB while VDC scans unlatched dynamic GTT surface).
 * Safe enable: boot-staging parameter santos_cand3b=1.
 * Safe rollback: restore baseline service script and clean reboot.
 */
static int santos_cand3b = 0;
module_param_named(santos_cand3b, santos_cand3b, int, 0444);
MODULE_PARM_DESC(santos_cand3b,
	"Santos Candidate 3B zero-copy scanout (1=dynamic GTT scanout, 0=A151f CPU copy fallback; load-time only)");

struct santos_cand3b_buf {
	PDC_MEM_INFO		psMemInfo;
	PPVRSRV_KERNEL_MEM_INFO	psKMemInfo;
	struct page		**ppsPages;
	IMG_UINT32		ui32PageCount;
	IMG_UINT32		ui32GttPteStart;
	IMG_UINT32		ui32GttApertureOffset;
	IMG_BOOL		bMapped;
};

struct santos_cand3b_retire_item {
	IMG_HANDLE		hCookie;
	u32			u32TargetFrame;
	struct santos_cand3b_buf *psBuf;
	bool			bValid;
};

struct santos_cand3b_state {
	IMG_BOOL		bInitialized;
	IMG_BOOL		bFailed;
	void __iomem		*pvVdcRegs;
	u32 __iomem		*pvGttMap;
	u16			u16OrigGmchCtrl;
	u32			u32OrigPgeCtl;
	struct page		*psScratchPage;
	u32			u32ScratchPte;

	struct santos_cand3b_buf asBuffers[SANTOS_CAND3B_MAX_BUFFERS];

	spinlock_t		sLock;

	/* Exactly ONE active scanout surface currently displayed */
	struct santos_cand3b_buf *psCurrentBuf;
	IMG_HANDLE		hCurrentCookie;

	/* FIFO queue for PREVIOUS scanout buffers waiting for replacement */
	struct santos_cand3b_retire_item asRetireQueue[SANTOS_CAND3B_RETIRE_QUEUE_SIZE];
	struct work_struct	sRetireWork;
	struct workqueue_struct	*psRetireWq;
	DC_NOHW_DEVINFO		*psDevInfo;

	IMG_UINT32		ui32FlipsPresented;
	IMG_UINT32		ui32FlipsRetiredImmediate;
	IMG_UINT32		ui32FlipsRetiredDelayed;
	IMG_UINT32		ui32RetireTimerFired;
	u32			u32WorkerStallCount;
	u32			u32WorkerPollCount;
	IMG_BOOL		bShuttingDown;
	IMG_BOOL		bObserverStalled;
};

/*
 * r8r1: sLock is statically initialized.  External teardown
 * (DestroyDCSwapChain()/Deinit()) may run before the first init or after an early init
 * failure (gpsPVRLDMDev NULL, ioremap failure, GTT base mismatch), so it must never
 * touch a runtime-initialized lock.  It is never re-initialized at runtime; re-arm after
 * a clean teardown reuses the same lock.
 */
static struct santos_cand3b_state sCand3bState = {
	.sLock = __SPIN_LOCK_UNLOCKED(sCand3bState.sLock),
};

/*
 * r8 operation gate: serializes Candidate 3B flip mapping/commit lifetime against
 * teardown (baseline latch + GTT wipe/unmap).  It is a file-scope static so it is usable
 * before santos_cand3b_init() has run (e.g. teardown on a partially initialized device).
 * Recursive acquisition is forbidden: try_flip()'s internal fail-closed transitions call
 * santos_cand3b_teardown_locked() while already holding the gate.
 */
static DEFINE_MUTEX(sCand3bOpMutex);

/*
 * r8 retirement observer (bounded frame-counter poll).
 *
 * Contract:
 *  - Completes PREVIOUS cookies only when the hardware frame counter is observed at or
 *    beyond their target (F_post + 1 of the successor write).  Never time-based.
 *  - NEVER touches psCurrentBuf/hCurrentCookie.
 *  - pfnPVRSRVCmdComplete(..., IMG_TRUE) is always called outside the spinlock: the worker
 *    is asynchronous workqueue context and is the ONLY Candidate 3B site that requests the
 *    Services MISR continuation (PVRSRVCommandCompleteKM -> OSScheduleMISR; golden vblank
 *    analogue MRSTFBFlipComplete -> pfnPVRSRVCmdComplete(..., MRST_TRUE)).  Every teardown
 *    context (external and internal) and the inline try_flip()/ProcessFlip drains pass
 *    IMG_FALSE.
 *  - Sleeps between polls (usleep_range) and holds no lock across the sleep.
 *  - Strict poll bound: if the counter stalls, pending cookies are preserved in the queue
 *    (later flips / teardown drain them safely); nothing is completed early.
 *  - r8 prompt exit: a queued or re-entered execution exits immediately when bShuttingDown
 *    or bObserverStalled is published; teardown/fail-closed then owns the pending cookies.
 *    It never keeps polling and never completes anything during a teardown transition.
 */
static void santos_cand3b_retire_worker(struct work_struct *work)
{
	unsigned long flags;
	u32 u32CurrFrame;
	IMG_HANDLE ahToComplete[SANTOS_CAND3B_RETIRE_QUEUE_SIZE + 2];
	u32 u32Polls;

	sCand3bState.ui32RetireTimerFired++;

	/* Invariant: psCurrentBuf is NEVER in the queue and is NEVER touched by this worker! */
	for (u32Polls = 0; u32Polls < SANTOS_CAND3B_RETIRE_MAX_POLLS; u32Polls++)
	{
		int nComplete = 0;
		int i;
		bool bStillPending = false;

		spin_lock_irqsave(&sCand3bState.sLock, flags);
		if (!sCand3bState.bInitialized || !sCand3bState.pvVdcRegs)
		{
			spin_unlock_irqrestore(&sCand3bState.sLock, flags);
			return;
		}

		/*
		 * r8 prompt exit: teardown published bShuttingDown, or the observer was already
		 * marked stalled.  Do not poll, do not touch the queue, do not complete: the
		 * teardown (or the next fail-closed flip) owns the pending cookies from here.
		 */
		if (sCand3bState.bShuttingDown || sCand3bState.bObserverStalled)
		{
			spin_unlock_irqrestore(&sCand3bState.sLock, flags);
			return;
		}

		u32CurrFrame = santos_cand3b_read_frame_counter(sCand3bState.pvVdcRegs);
		sCand3bState.u32WorkerPollCount++;

		/* Check pending items in retirement queue (PREVIOUS scanout buffers only) */
		for (i = 0; i < SANTOS_CAND3B_RETIRE_QUEUE_SIZE; i++)
		{
			if (sCand3bState.asRetireQueue[i].bValid)
			{
				if (santos_cand3b_frame_passed(u32CurrFrame,
							       sCand3bState.asRetireQueue[i].u32TargetFrame))
				{
					if (nComplete < (int)ARRAY_SIZE(ahToComplete))
					{
						ahToComplete[nComplete++] =
							sCand3bState.asRetireQueue[i].hCookie;
					}
					sCand3bState.asRetireQueue[i].bValid = false;
					sCand3bState.asRetireQueue[i].hCookie = NULL;
					sCand3bState.asRetireQueue[i].psBuf = NULL;
					sCand3bState.ui32FlipsRetiredDelayed++;
				}
				else
				{
					bStillPending = true;
				}
			}
		}

		spin_unlock_irqrestore(&sCand3bState.sLock, flags);

		/* Complete retired cookies OUTSIDE the spinlock, as soon as their target passed.
		 * IMG_TRUE requests the asynchronous Services MISR continuation (the only such
		 * Candidate 3B site), matching the golden vblank completion path (MRST_TRUE). */
		for (i = 0; i < nComplete; i++)
		{
			if (ahToComplete[i] && sCand3bState.psDevInfo)
			{
				sCand3bState.psDevInfo->sPVRJTable.pfnPVRSRVCmdComplete(ahToComplete[i], IMG_TRUE);
			}
		}

		if (!bStillPending)
		{
			sCand3bState.u32WorkerStallCount = 0;
			return;
		}

		/* No lock is held here.  Poll again promptly (1-2 ms) until the target passes. */
		if (u32Polls + 1 >= SANTOS_CAND3B_RETIRE_MAX_POLLS)
		{
			break;	/* strict bound: do not sleep one extra quantum */
		}
		usleep_range(SANTOS_CAND3B_RETIRE_POLL_US_MIN, SANTOS_CAND3B_RETIRE_POLL_US_MAX);
	}

	/* Strict bound expired: counter stalled.  Preserve pending cookies; NEVER complete early.
	 * Mark the observer stalled under the lock: no further flip may be accepted into
	 * Candidate 3B while an unpassed retirement entry is stranded; the next try_flip()
	 * performs a fail-closed teardown to the baseline. */
	spin_lock_irqsave(&sCand3bState.sLock, flags);
	sCand3bState.u32WorkerStallCount++;
	sCand3bState.bObserverStalled = IMG_TRUE;
	spin_unlock_irqrestore(&sCand3bState.sLock, flags);
	pr_warn_ratelimited("santos-cand3b: retire observer stalled (%u polls); pending cookies preserved, failing closed on next flip\n",
			    u32Polls);
}

static void santos_cand3b_schedule_retire(void)
{
	if (sCand3bState.psRetireWq && sCand3bState.bInitialized &&
	    !sCand3bState.bShuttingDown)
	{
		queue_work(sCand3bState.psRetireWq, &sCand3bState.sRetireWork);
	}
}

static bool santos_cand3b_init(DC_NOHW_DEVINFO *psDevInfo)
{
	struct pci_dev *pdev = gpsPVRLDMDev;
	resource_size_t vdc_phys;
	u32 reg_pgetbl;
	u32 phys_gtt_base;
	u32 i;

	if (sCand3bState.bInitialized)
		return true;
	if (sCand3bState.bFailed)
		return false;

	if (!pdev)
	{
		pr_err("santos-cand3b: gpsPVRLDMDev is NULL\n");
		sCand3bState.bFailed = IMG_TRUE;
		return false;
	}

	vdc_phys = pci_resource_start(pdev, 0);
	if (!vdc_phys)
	{
		pr_err("santos-cand3b: failed to get BAR0 phys address\n");
		sCand3bState.bFailed = IMG_TRUE;
		return false;
	}

	sCand3bState.pvVdcRegs = ioremap(vdc_phys, 0x80000);
	if (!sCand3bState.pvVdcRegs)
	{
		pr_err("santos-cand3b: failed to ioremap BAR0 VDC registers\n");
		sCand3bState.bFailed = IMG_TRUE;
		return false;
	}

	reg_pgetbl = ioread32(sCand3bState.pvVdcRegs + SANTOS_VDC_PGETBL_CTL);
	phys_gtt_base = reg_pgetbl & PAGE_MASK;
	if (phys_gtt_base != 0x3ffc0000u)
	{
		pr_err("santos-cand3b: unexpected GTT phys base 0x%08x (expected 0x3ffc0000)\n",
		       phys_gtt_base);
		iounmap(sCand3bState.pvVdcRegs);
		sCand3bState.pvVdcRegs = NULL;
		sCand3bState.bFailed = IMG_TRUE;
		return false;
	}

	sCand3bState.pvGttMap = ioremap(phys_gtt_base, 256 * 1024);
	if (!sCand3bState.pvGttMap)
	{
		pr_err("santos-cand3b: failed to ioremap GTT table at 0x%08x\n", phys_gtt_base);
		iounmap(sCand3bState.pvVdcRegs);
		sCand3bState.pvVdcRegs = NULL;
		sCand3bState.bFailed = IMG_TRUE;
		return false;
	}

	/* Allocate dedicated scratch page (matches golden dev_priv->scratch_page).
	 * r8: a failed allocation must fail Candidate 3B initialization instead of
	 * programming zero/invalid PTEs into the dynamic GTT region. */
	sCand3bState.psScratchPage = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!sCand3bState.psScratchPage)
	{
		pr_err("santos-cand3b: failed to allocate scratch page; refusing to initialize Candidate 3B\n");
		iounmap(sCand3bState.pvGttMap);
		sCand3bState.pvGttMap = NULL;
		iounmap(sCand3bState.pvVdcRegs);
		sCand3bState.pvVdcRegs = NULL;
		sCand3bState.bFailed = IMG_TRUE;
		return false;
	}
	{
		u32 pfn = page_to_pfn(sCand3bState.psScratchPage);
		sCand3bState.u32ScratchPte = (pfn << PAGE_SHIFT) | 0x00000001u;
	}

	/* r8r1: allocate the retirement workqueue BEFORE publishing any dynamic GTT PTE.
	 * It is the last fallible resource; with this ordering the only failure path that
	 * frees the scratch page runs while no PTE references it.  A failure here unwinds
	 * completely (scratch page + both ioremaps) with the dynamic region untouched. */
	sCand3bState.psRetireWq = alloc_ordered_workqueue("santos-cand3b", 0);
	if (!sCand3bState.psRetireWq)
	{
		pr_err("santos-cand3b: failed to allocate retirement workqueue\n");
		__free_page(sCand3bState.psScratchPage);
		sCand3bState.psScratchPage = NULL;
		iounmap(sCand3bState.pvGttMap);
		sCand3bState.pvGttMap = NULL;
		iounmap(sCand3bState.pvVdcRegs);
		sCand3bState.pvVdcRegs = NULL;
		sCand3bState.bFailed = IMG_TRUE;
		return false;
	}

	/* Publish the scratch mapping in the dynamic region [4031 .. 4031+8000-1].
	 * No later fallible step remains (the GMCH/PGETBL enable writes below cannot fail),
	 * so no dynamic PTE can ever reference a freed scratch page. */
	for (i = 0; i < SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF; i++)
	{
		iowrite32(sCand3bState.u32ScratchPte,
			  &sCand3bState.pvGttMap[SANTOS_CAND3B_GTT_BASE_PTE + i]);
	}
	(void)ioread32(&sCand3bState.pvGttMap[SANTOS_CAND3B_GTT_BASE_PTE +
		       SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF - 1]);

	/* Read original hardware state */
	pci_read_config_word(pdev, SANTOS_PCI_GMCH_CTRL, &sCand3bState.u16OrigGmchCtrl);
	sCand3bState.u32OrigPgeCtl = reg_pgetbl;

	/* Golden enable order from psb_gtt_init (psb_gtt.c:90-97):
	 * 1. Write GMCH_CTRL with _PSB_GMCH_ENABLED (0x4, bit 2)
	 * 2. Write PSB_PGETBL_CTL with _PSB_PGETBL_ENABLED (0x1, bit 0)
	 * 3. Posting read of PSB_PGETBL_CTL
	 */
	pci_write_config_word(pdev, SANTOS_PCI_GMCH_CTRL,
			      sCand3bState.u16OrigGmchCtrl | _PSB_GMCH_ENABLED);

	iowrite32(sCand3bState.u32OrigPgeCtl | _PSB_PGETBL_ENABLED,
		  sCand3bState.pvVdcRegs + SANTOS_VDC_PGETBL_CTL);
	(void)ioread32(sCand3bState.pvVdcRegs + SANTOS_VDC_PGETBL_CTL);

	INIT_WORK(&sCand3bState.sRetireWork, santos_cand3b_retire_worker);
	sCand3bState.psDevInfo = psDevInfo;

	sCand3bState.bShuttingDown = IMG_FALSE;
	sCand3bState.bObserverStalled = IMG_FALSE;
	sCand3bState.bInitialized = IMG_TRUE;

	pr_info("santos-cand3b: Candidate 3B INITIALIZED (GTT base=0x%08x, dynamic slots %u..%u, GMCH 0x%04x->0x%04x, PGETBL 0x%08x->0x%08x)\n",
		phys_gtt_base, SANTOS_CAND3B_GTT_BASE_PTE,
		SANTOS_CAND3B_GTT_BASE_PTE + SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF - 1,
		sCand3bState.u16OrigGmchCtrl, (u16)(sCand3bState.u16OrigGmchCtrl | _PSB_GMCH_ENABLED),
		sCand3bState.u32OrigPgeCtl, (u32)(sCand3bState.u32OrigPgeCtl | _PSB_PGETBL_ENABLED));

	return true;
}

/* Forward declaration: the gate-taking wrapper below calls the _locked body defined next. */
static void santos_cand3b_teardown_locked(IMG_BOOL bScheduleMISR);

/*
 * r8/r8r1 teardown serialization.
 *
 * teardown is split into a gate-taking wrapper and a _locked body:
 *  - try_flip() holds the operation gate for its entire mapping/commit lifetime and calls
 *    the _locked variant on its own fail-closed transitions (no recursive gate acquisition);
 *  - external callers (DestroyDCSwapChain/Deinit) use the wrapper.  The wrapper publishes
 *    bShuttingDown under sLock BEFORE acquiring the gate, so an in-flight flip observes it in
 *    the final admission check and declines to commit a new scanout; the wrapper then waits
 *    for that flip to finish before touching the GTT (wipe/unmap).
 *
 * Services completion scheduling context (PVRSRVCommandCompleteKM, queue.c:1402-1514):
 *  - the completion itself (src/dst sync updates, sync ref release, command callback,
 *    native-sync timeline increment/cleanup-fence put, bInUse clear, device callbacks)
 *    always happens regardless of the flag;
 *  - IMG_TRUE additionally calls OSScheduleMISR(); only the asynchronous retirement
 *    worker needs that, so queued display work continues;
 *  - ALL teardown contexts pass IMG_FALSE, including external DestroyDCSwapChain/Deinit:
 *    a terminal teardown must not request another Services queue-processing pass while
 *    swapchain/command-processor structures are being freed.  The explicit bScheduleMISR
 *    argument is retained for clarity; both current callers pass IMG_FALSE.
 */
static void santos_cand3b_teardown(void)
{
	unsigned long flags;

	spin_lock_irqsave(&sCand3bState.sLock, flags);
	sCand3bState.bShuttingDown = IMG_TRUE;
	spin_unlock_irqrestore(&sCand3bState.sLock, flags);

	/* Wait for any in-flight try_flip mapping/commit before wiping GTT/DSPASURF. */
	mutex_lock(&sCand3bOpMutex);
	santos_cand3b_teardown_locked(IMG_FALSE);
	mutex_unlock(&sCand3bOpMutex);
}

static void santos_cand3b_teardown_locked(IMG_BOOL bScheduleMISR)
{
	unsigned long flags;
	IMG_HANDLE ahToComplete[SANTOS_CAND3B_RETIRE_QUEUE_SIZE + 2];
	int nComplete = 0;
	IMG_UINT32 i;
	u32 u32FrameBase, u32TargetBaseline, u32CurrentFrame = 0;
	bool bLatchSuccess = true;

	if (!sCand3bState.bInitialized)
		return;

	/* 0. Mark shutting down under the lock: prevents a concurrent flip path from
	 *    queueing new retirement work across cancel_work_sync()/destroy_workqueue(). */
	spin_lock_irqsave(&sCand3bState.sLock, flags);
	sCand3bState.bShuttingDown = IMG_TRUE;
	spin_unlock_irqrestore(&sCand3bState.sLock, flags);

	/* 1. Cancel the retire observer work (waits for a running poll bounded by MAX_POLLS) */
	cancel_work_sync(&sCand3bState.sRetireWork);

	/* 2. Return display plane to known baseline surface (DSPASURF = 0) */
	if (sCand3bState.pvVdcRegs && sCand3bState.psCurrentBuf)
	{
		iowrite32(0x00000000u, sCand3bState.pvVdcRegs + SANTOS_VDC_DSPASURF);
		(void)ioread32(sCand3bState.pvVdcRegs + SANTOS_VDC_DSPASURF);

		/* 3. Wait for hardware to latch baseline surface (>= 2 frames, max 60 ms) */
		u32FrameBase = santos_cand3b_read_frame_counter(sCand3bState.pvVdcRegs);
		u32TargetBaseline = (u32FrameBase + 2) & 0x00ffffffu;
		bLatchSuccess = false;
		for (i = 0; i < 60; i++)
		{
			msleep(1);
			u32CurrentFrame = santos_cand3b_read_frame_counter(sCand3bState.pvVdcRegs);
			if (santos_cand3b_frame_passed(u32CurrentFrame, u32TargetBaseline))
			{
				bLatchSuccess = true;
				break;
			}
		}

		if (!bLatchSuccess)
		{
			pr_err("santos-cand3b: TEARDOWN TIMEOUT waiting for baseline latch: base=%u target=%u curr=%u (waited 60ms)\n",
			       u32FrameBase, u32TargetBaseline, u32CurrentFrame);
			pr_err("santos-cand3b: FAILING CLOSED: preserving CURRENT buffer and GTT mapping to prevent Display UAF!\n");
			sCand3bState.bFailed = IMG_TRUE;
		}
	}

	/* 4. Complete pending cookies */
	spin_lock_irqsave(&sCand3bState.sLock, flags);
	if (bLatchSuccess)
	{
		/* Hardware verified latched back to baseline: all cookies can be completed */
		for (i = 0; i < SANTOS_CAND3B_RETIRE_QUEUE_SIZE; i++)
		{
			if (sCand3bState.asRetireQueue[i].bValid && sCand3bState.asRetireQueue[i].hCookie)
			{
				if (nComplete < (int)ARRAY_SIZE(ahToComplete))
				{
					ahToComplete[nComplete++] = sCand3bState.asRetireQueue[i].hCookie;
				}
				sCand3bState.asRetireQueue[i].bValid = false;
				sCand3bState.asRetireQueue[i].hCookie = NULL;
				sCand3bState.asRetireQueue[i].psBuf = NULL;
			}
		}
		if (sCand3bState.hCurrentCookie)
		{
			if (nComplete < (int)ARRAY_SIZE(ahToComplete))
			{
				ahToComplete[nComplete++] = sCand3bState.hCurrentCookie;
			}
			sCand3bState.hCurrentCookie = NULL;
			sCand3bState.psCurrentBuf = NULL;
		}
	}
	else
	{
		/*
		 * TIMEOUT FAIL-CLOSED:
		 * Hardware did not latch baseline. VDC may still scan out from psCurrentBuf.
		 * Complete ONLY queue items whose target frames actually passed before timeout.
		 * CURRENT buffer (hCurrentCookie) is deliberately NOT completed.
		 */
		for (i = 0; i < SANTOS_CAND3B_RETIRE_QUEUE_SIZE; i++)
		{
			if (sCand3bState.asRetireQueue[i].bValid && sCand3bState.asRetireQueue[i].hCookie)
			{
				if (santos_cand3b_frame_passed(u32CurrentFrame,
							       sCand3bState.asRetireQueue[i].u32TargetFrame))
				{
					if (nComplete < (int)ARRAY_SIZE(ahToComplete))
					{
						ahToComplete[nComplete++] = sCand3bState.asRetireQueue[i].hCookie;
					}
					sCand3bState.asRetireQueue[i].bValid = false;
					sCand3bState.asRetireQueue[i].hCookie = NULL;
					sCand3bState.asRetireQueue[i].psBuf = NULL;
				}
			}
		}
	}
	spin_unlock_irqrestore(&sCand3bState.sLock, flags);

	/* Complete outside spinlock; r8r2 passes IMG_FALSE for every teardown context
	 * (external and internal): the full completion bookkeeping still runs, but no new
	 * Services MISR may be scheduled while the display lifecycle is being dismantled. */
	for (i = 0; i < (IMG_UINT32)nComplete; i++)
	{
		if (ahToComplete[i] && sCand3bState.psDevInfo)
		{
			sCand3bState.psDevInfo->sPVRJTable.pfnPVRSRVCmdComplete(ahToComplete[i], bScheduleMISR);
		}
	}

	if (!bLatchSuccess)
	{
		/* Fail closed: do NOT wipe GTT, do NOT disable GMCH/PGETBL, do NOT free scratch page */
		pr_err("santos-cand3b: teardown completed in FAIL-CLOSED state (resources retained)\n");
		return;
	}

	/* 5. Restore dynamic GTT slots to scratch/unmapped */
	if (sCand3bState.pvGttMap)
	{
		for (i = 0; i < SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF; i++)
		{
			iowrite32(sCand3bState.u32ScratchPte,
				  &sCand3bState.pvGttMap[SANTOS_CAND3B_GTT_BASE_PTE + i]);
		}
		(void)ioread32(&sCand3bState.pvGttMap[SANTOS_CAND3B_GTT_BASE_PTE +
			       SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF - 1]);
	}

	/* 6. Restore original GMCH and PGETBL registers */
	if (gpsPVRLDMDev && sCand3bState.pvVdcRegs)
	{
		pci_write_config_word(gpsPVRLDMDev, SANTOS_PCI_GMCH_CTRL, sCand3bState.u16OrigGmchCtrl);
		iowrite32(sCand3bState.u32OrigPgeCtl, sCand3bState.pvVdcRegs + SANTOS_VDC_PGETBL_CTL);
		(void)ioread32(sCand3bState.pvVdcRegs + SANTOS_VDC_PGETBL_CTL);
	}

	/* 6b. r8r1 safe unwind: the dynamic translation is disabled now, so clear the retained
	 *     dynamic PTEs before freeing the scratch page they currently reference.  A later
	 *     re-init republishes a fresh scratch PTE for every slot before re-enabling. */
	if (sCand3bState.pvGttMap)
	{
		for (i = 0; i < SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF; i++)
		{
			iowrite32(0x00000000u,
				  &sCand3bState.pvGttMap[SANTOS_CAND3B_GTT_BASE_PTE + i]);
		}
		(void)ioread32(&sCand3bState.pvGttMap[SANTOS_CAND3B_GTT_BASE_PTE +
			       SANTOS_CAND3B_MAX_BUFFERS * SANTOS_CAND3B_PAGES_PER_BUF - 1]);
	}

	/* 7. Free scratch page (no retained PTE references it any more) */
	if (sCand3bState.psScratchPage)
	{
		__free_page(sCand3bState.psScratchPage);
		sCand3bState.psScratchPage = NULL;
	}

	/* 8. Unmap resources */
	if (sCand3bState.pvGttMap)
	{
		iounmap(sCand3bState.pvGttMap);
		sCand3bState.pvGttMap = NULL;
	}
	if (sCand3bState.pvVdcRegs)
	{
		iounmap(sCand3bState.pvVdcRegs);
		sCand3bState.pvVdcRegs = NULL;
	}

	/* 9. Destroy the r7 retirement workqueue (clean path only; the fail-closed path
	 *    returns earlier and intentionally retains it).  Work was cancelled in step 1. */
	if (sCand3bState.psRetireWq)
	{
		destroy_workqueue(sCand3bState.psRetireWq);
		sCand3bState.psRetireWq = NULL;
	}

	memset(sCand3bState.asBuffers, 0, sizeof(sCand3bState.asBuffers));
	sCand3bState.bInitialized = IMG_FALSE;
	sCand3bState.bFailed = IMG_FALSE;
	/* r8: clean teardown is complete and no producer can queue work (gate held, workqueue
	 * destroyed); clear the shutdown flag so a later re-init starts from a clean state. */
	sCand3bState.bShuttingDown = IMG_FALSE;

	pr_info("santos-cand3b: teardown complete (restored baseline surface, GMCH/PGETBL restored)\n");
}

static struct santos_cand3b_buf *
santos_cand3b_get_or_map_buf(DISPLAYCLASS_FLIP_COMMAND2 *psFlipCmd2)
{
	PDC_MEM_INFO psMemInfo;
	PPVRSRV_KERNEL_MEM_INFO psKMemInfo;
	LinuxMemArea *psArea;
	struct page **ppsPages;
	struct santos_cand3b_buf *psBuf;
	IMG_UINT32 i;
	int iSlot = -1;

	size_t uByteSize;
	IMG_UINT32 ui32PageCount;

	if (!psFlipCmd2 || psFlipCmd2->ui32NumMemInfos != 1 || !psFlipCmd2->ppsMemInfos)
	{
		return IMG_NULL;
	}

	psMemInfo = psFlipCmd2->ppsMemInfos[0];
	if (!psMemInfo ||
	    (psMemInfo->memType != PVRSRV_MEMTYPE_DEVICE &&
	     psMemInfo->memType != PVRSRV_MEMTYPE_DEVICECLASS))
	{
		pr_err_ratelimited("santos-cand3b: rejected unsupported memType %d\n",
				   psMemInfo ? psMemInfo->memType : -1);
		return IMG_NULL;
	}

	psKMemInfo = (PPVRSRV_KERNEL_MEM_INFO)psMemInfo;
	if (!psKMemInfo || !psKMemInfo->sMemBlk.hOSMemHandle)
	{
		return IMG_NULL;
	}

	/* Check if already registered */
	for (i = 0; i < SANTOS_CAND3B_MAX_BUFFERS; i++)
	{
		if (sCand3bState.asBuffers[i].bMapped && sCand3bState.asBuffers[i].psMemInfo == psMemInfo)
		{
			return &sCand3bState.asBuffers[i];
		}
	}

	/*
	 * 1. Validate underlying memory area type and writecombining flags
	 */
	psArea = (LinuxMemArea *)psKMemInfo->sMemBlk.hOSMemHandle;
	if (!psArea || psArea->eAreaType != LINUX_MEM_AREA_ALLOC_PAGES)
	{
		pr_err("santos-cand3b: invalid LinuxMemArea type %d\n",
		       psArea ? psArea->eAreaType : -1);
		return IMG_NULL;
	}

	if (!(psArea->ui32AreaFlags & PVRSRV_HAP_WRITECOMBINE) ||
	    !(psKMemInfo->ui32Flags & PVRSRV_HAP_WRITECOMBINE))
	{
		pr_err("santos-cand3b: buffer allocation not writecombined\n");
		return IMG_NULL;
	}

	/*
	 * 2. Authoritative size validation:
	 *    Read size from both KMemInfo (PVR services allocation metadata)
	 *    and psArea (OS page allocator metadata) and verify equality.
	 */
	uByteSize = psArea->uiByteSize;
	if (psKMemInfo->uAllocSize != uByteSize)
	{
		pr_err("santos-cand3b: size mismatch: KMemInfo uAllocSize 0x%zx != Area uiByteSize 0x%zx\n",
		       (size_t)psKMemInfo->uAllocSize, uByteSize);
		return IMG_NULL;
	}

	/*
	 * 3. Partial page / alignment validation: size must be non-zero and strictly page-aligned
	 */
	if (uByteSize == 0 || (uByteSize & (PAGE_SIZE - 1)) != 0)
	{
		pr_err("santos-cand3b: buffer size 0x%zx is not non-zero page-aligned\n", uByteSize);
		return IMG_NULL;
	}

	/*
	 * 4. Authoritatively derive page count from byte size
	 */
	ui32PageCount = (IMG_UINT32)(uByteSize >> PAGE_SHIFT);

	/*
	 * 5. Scanout geometry invariant: VDC hardware plane is configured for
	 *    1280 x 800 x 4 bytes = 4,096,000 bytes = exactly 1000 pages (SANTOS_A151F_SIZE).
	 *    Reject any undersized or oversized buffer BEFORE touching page list.
	 */
	if (uByteSize != SANTOS_A151F_SIZE || ui32PageCount != SANTOS_CAND3B_PAGES_PER_BUF)
	{
		pr_err("santos-cand3b: buffer size 0x%zx (%u pages) != required scanout size 0x%x (%u pages)\n",
		       uByteSize, ui32PageCount, SANTOS_A151F_SIZE, SANTOS_CAND3B_PAGES_PER_BUF);
		return IMG_NULL;
	}

	/*
	 * 6. Validate page list pointer
	 */
	ppsPages = psArea->uData.sPageList.ppsPageList;
	if (!ppsPages)
	{
		pr_err("santos-cand3b: NULL ppsPageList\n");
		return IMG_NULL;
	}

	/*
	 * 7. Audit every page entry: verify non-NULL and valid PFN before any PTE write
	 */
	for (i = 0; i < ui32PageCount; i++)
	{
		struct page *p = ppsPages[i];
		if (!p)
		{
			pr_err("santos-cand3b: NULL page entry at index %u of %u\n", i, ui32PageCount);
			return IMG_NULL;
		}
		if (!pfn_valid(page_to_pfn(p)))
		{
			pr_err("santos-cand3b: invalid pfn 0x%lx at page index %u\n",
			       (unsigned long)page_to_pfn(p), i);
			return IMG_NULL;
		}
	}

	/* Find a free slot in asBuffers */
	for (i = 0; i < SANTOS_CAND3B_MAX_BUFFERS; i++)
	{
		if (!sCand3bState.asBuffers[i].bMapped)
		{
			iSlot = (int)i;
			break;
		}
	}
	if (iSlot < 0)
	{
		pr_err("santos-cand3b: all %u buffer slots exhausted\n", SANTOS_CAND3B_MAX_BUFFERS);
		return IMG_NULL;
	}

	psBuf = &sCand3bState.asBuffers[iSlot];

	/*
	 * 8. Validate GTT slot capacity and dynamic pool bounds:
	 *    Page count cannot exceed per-slot capacity or dynamic pool boundary.
	 */
	psBuf->ui32GttPteStart = SANTOS_CAND3B_GTT_BASE_PTE + (IMG_UINT32)iSlot * SANTOS_CAND3B_PAGES_PER_BUF;
	if (ui32PageCount > SANTOS_CAND3B_PAGES_PER_BUF ||
	    psBuf->ui32GttPteStart + ui32PageCount > SANTOS_CAND3B_GTT_POOL_END_PTE)
	{
		pr_err("santos-cand3b: slot %d GTT range [%u..%u] exceeds dynamic pool limit %u\n",
		       iSlot, psBuf->ui32GttPteStart, psBuf->ui32GttPteStart + ui32PageCount,
		       SANTOS_CAND3B_GTT_POOL_END_PTE);
		return IMG_NULL;
	}

	psBuf->psMemInfo = psMemInfo;
	psBuf->psKMemInfo = psKMemInfo;
	psBuf->ppsPages = ppsPages;
	psBuf->ui32PageCount = ui32PageCount;
	psBuf->ui32GttApertureOffset = psBuf->ui32GttPteStart * PAGE_SIZE;

	/*
	 * 9. Program GTT PTEs using golden psb_gtt_mask_pte(pfn, 0) encoding
	 *    (performed only after ALL validations pass):
	 *    pte = (pfn << PAGE_SHIFT) | 0x00000001
	 */
	for (i = 0; i < ui32PageCount; i++)
	{
		u32 pfn = page_to_pfn(ppsPages[i]);
		u32 pte = (pfn << PAGE_SHIFT) | 0x00000001u;
		iowrite32(pte, &sCand3bState.pvGttMap[psBuf->ui32GttPteStart + i]);
	}
	/* Golden posting read of last PTE */
	(void)ioread32(&sCand3bState.pvGttMap[psBuf->ui32GttPteStart + ui32PageCount - 1]);

	psBuf->bMapped = IMG_TRUE;

	pr_info("santos-cand3b: mapped buffer[%d] psMemInfo=%px (%u pages, 0x%zx bytes) to GTT PTEs %u..%u (offset 0x%08x)\n",
		iSlot, psMemInfo, ui32PageCount, uByteSize, psBuf->ui32GttPteStart,
		psBuf->ui32GttPteStart + ui32PageCount - 1,
		psBuf->ui32GttApertureOffset);

	return psBuf;
}

static void santos_cand3b_retire_check_locked(u32 u32CurrentFrame,
					      IMG_HANDLE *ahToComplete,
					      int *pnComplete,
					      int maxComplete)
{
	int i;
	for (i = 0; i < SANTOS_CAND3B_RETIRE_QUEUE_SIZE; i++)
	{
		if (sCand3bState.asRetireQueue[i].bValid)
		{
			if (santos_cand3b_frame_passed(u32CurrentFrame,
						       sCand3bState.asRetireQueue[i].u32TargetFrame))
			{
				if (*pnComplete < maxComplete)
				{
					ahToComplete[(*pnComplete)++] =
						sCand3bState.asRetireQueue[i].hCookie;
				}
				sCand3bState.asRetireQueue[i].bValid = false;
				sCand3bState.asRetireQueue[i].hCookie = NULL;
				sCand3bState.asRetireQueue[i].psBuf = NULL;
				sCand3bState.ui32FlipsRetiredImmediate++;
			}
		}
	}
}

static bool santos_cand3b_retire_enqueue_locked(IMG_HANDLE hCookie,
						struct santos_cand3b_buf *psBuf,
						u32 u32TargetFrame)
{
	int i;
	int iFree = -1;

	for (i = 0; i < SANTOS_CAND3B_RETIRE_QUEUE_SIZE; i++)
	{
		if (!sCand3bState.asRetireQueue[i].bValid)
		{
			iFree = i;
			break;
		}
	}

	if (iFree < 0)
	{
		/* Queue full: reject enqueue to prevent dropping unpassed cookies */
		return false;
	}

	sCand3bState.asRetireQueue[iFree].hCookie = hCookie;
	sCand3bState.asRetireQueue[iFree].psBuf = psBuf;
	sCand3bState.asRetireQueue[iFree].u32TargetFrame = u32TargetFrame;
	sCand3bState.asRetireQueue[iFree].bValid = true;
	return true;
}

static IMG_BOOL santos_cand3b_try_flip(DC_NOHW_DEVINFO *psDevInfo,
				       DISPLAYCLASS_FLIP_COMMAND2 *psFlipCmd2,
				       IMG_HANDLE hCmdCookie)
{
	struct santos_cand3b_buf *psNewBuf;
	struct santos_cand3b_buf *psPrevBuf = NULL;
	IMG_HANDLE hPrevCookie = NULL;
	unsigned long flags;
	u32 u32FramePost = 0;
	u32 u32TargetRetire = 0;
	IMG_HANDLE ahToComplete[SANTOS_CAND3B_RETIRE_QUEUE_SIZE + 2];
	int nComplete = 0;
	int i;
	bool bHavePendingRetire = false;
	bool bFinalStalled = false;

	/*
	 * r8: hold the operation gate across the entire flip (buffer mapping + DSPASURF
	 * commit).  teardown() publishes bShuttingDown first and then takes this same gate,
	 * so the GTT cannot be wiped/unmapped and GMCH/PGETBL cannot be restored while this
	 * flip is still writing PTEs or programming DSPASURF.  The gate is never re-acquired
	 * here: internal fail-closed transitions use santos_cand3b_teardown_locked().
	 */
	mutex_lock(&sCand3bOpMutex);

	/* Admission re-check under the gate: a concurrent teardown may have completed while
	 * this flip was waiting for the gate. */
	if (sCand3bState.bFailed)
	{
		mutex_unlock(&sCand3bOpMutex);
		return IMG_FALSE;
	}

	/*
	 * r7 fail-closed transition (preserved in r8): if the bounded observer stalled with an
	 * unpassed PREVIOUS entry still queued, no new flip may enter Candidate 3B.  Perform
	 * the normal internal teardown here (process context; the observer work has already
	 * returned, so cancel_work_sync cannot deadlock) and fall through to A151f.
	 * teardown_locked() returns to baseline and completes all cookies after the latch, or
	 * fails closed (retaining CURRENT/pending and the GTT mapping) on a stalled pipe.
	 */
	if (sCand3bState.bObserverStalled)
	{
		pr_err("santos-cand3b: retire observer stalled with pending buffers; failing closed to baseline\n");
		santos_cand3b_teardown_locked(IMG_FALSE);
		sCand3bState.bFailed = IMG_TRUE;
		mutex_unlock(&sCand3bOpMutex);
		return IMG_FALSE;
	}

	if (!sCand3bState.bInitialized)
	{
		if (!santos_cand3b_init(psDevInfo))
		{
			mutex_unlock(&sCand3bOpMutex);
			return IMG_FALSE;
		}
	}
	else if (sCand3bState.bShuttingDown)
	{
		/* A teardown has published its intent and is waiting for this gate: refuse
		 * before mapping anything. */
		mutex_unlock(&sCand3bOpMutex);
		return IMG_FALSE;
	}

	psNewBuf = santos_cand3b_get_or_map_buf(psFlipCmd2);
	if (!psNewBuf)
	{
		/*
		 * If a Candidate 3B surface is currently active on DSPASURF,
		 * we cannot simply return IMG_FALSE and let A151f write to baseline physical FB
		 * while VDC continues scanning the experimental surface.
		 * Cleanly tear down Candidate 3B back to baseline DSPASURF=0 first.
		 */
		if (sCand3bState.psCurrentBuf)
		{
			pr_warn("santos-cand3b: buffer map failed with active surface; tearing down to baseline\n");
			santos_cand3b_teardown_locked(IMG_FALSE);
			sCand3bState.bFailed = IMG_TRUE;
		}
		mutex_unlock(&sCand3bOpMutex);
		return IMG_FALSE;
	}

	/*
	 * Rapid-flip / queue-overflow protection:
	 * If we have an active previous scanout buffer that will need to be enqueued,
	 * ensure the retirement queue has space BEFORE writing DSPASURF.
	 * Aggressively drain any already-retired buffers first.
	 */
	if (sCand3bState.psCurrentBuf)
	{
		bool bHaveSlot = false;
		int wait_iter;

		for (wait_iter = 0; wait_iter < 20; wait_iter++)
		{
			u32 u32FramePre = santos_cand3b_read_frame_counter(sCand3bState.pvVdcRegs);

			spin_lock_irqsave(&sCand3bState.sLock, flags);
			santos_cand3b_retire_check_locked(u32FramePre, ahToComplete, &nComplete,
							  (int)ARRAY_SIZE(ahToComplete));
			for (i = 0; i < SANTOS_CAND3B_RETIRE_QUEUE_SIZE; i++)
			{
				if (!sCand3bState.asRetireQueue[i].bValid)
				{
					bHaveSlot = true;
					break;
				}
			}
			spin_unlock_irqrestore(&sCand3bState.sLock, flags);

			/* Complete any drained cookies outside spinlock; this runs inline inside
			 * the active ProcessFlip()/Services queue processing, so it must pass
			 * IMG_FALSE (no MISR scheduling; the queue processor continues after
			 * ProcessFlip returns). */
			for (i = 0; i < nComplete; i++)
			{
				if (ahToComplete[i])
				{
					psDevInfo->sPVRJTable.pfnPVRSRVCmdComplete(ahToComplete[i], IMG_FALSE);
				}
			}
			nComplete = 0;

			if (bHaveSlot)
				break;

			msleep(1);
		}

		if (!bHaveSlot)
		{
			pr_err("santos-cand3b: RETIRE QUEUE OVERFLOW: all %u slots occupied; failing closed to baseline\n",
			       SANTOS_CAND3B_RETIRE_QUEUE_SIZE);
			santos_cand3b_teardown_locked(IMG_FALSE);
			sCand3bState.bFailed = IMG_TRUE;
			mutex_unlock(&sCand3bOpMutex);
			return IMG_FALSE;
		}
	}

	spin_lock_irqsave(&sCand3bState.sLock, flags);

	/*
	 * r8 FINAL ADMISSION CHECK (race-safe, under sLock): teardown publishes bShuttingDown
	 * under this same lock; the retire worker publishes bObserverStalled under it; the
	 * fail-closed transitions set bFailed.  Checking immediately before the DSPASURF commit
	 * closes the window between the entry checks / buffer mapping and the scanout commit.
	 * The commit below stays inside this same critical section, so the worker cannot mark
	 * the observer stalled "just after" the check and still race a committed scanout.
	 */
	if (sCand3bState.bShuttingDown || sCand3bState.bObserverStalled ||
	    sCand3bState.bFailed)
	{
		bFinalStalled = (sCand3bState.bObserverStalled != IMG_FALSE);
		spin_unlock_irqrestore(&sCand3bState.sLock, flags);

		if (bFinalStalled)
		{
			/* The observer stalled while this flip was mapping: abort the commit and
			 * run the fail-closed transition now (the worker has already returned). */
			pr_err("santos-cand3b: observer stalled before commit; failing closed to baseline\n");
			santos_cand3b_teardown_locked(IMG_FALSE);
			sCand3bState.bFailed = IMG_TRUE;
		}
		mutex_unlock(&sCand3bOpMutex);
		return IMG_FALSE;
	}

	/* 1. Program DSPASURF with the newly presented buffer's GTT aperture offset */
	iowrite32(psNewBuf->ui32GttApertureOffset, sCand3bState.pvVdcRegs + SANTOS_VDC_DSPASURF);

	/* 2. Posted-write readback */
	(void)ioread32(sCand3bState.pvVdcRegs + SANTOS_VDC_DSPASURF);

	/* 3. Coherently sample hardware frame counter AFTER the write */
	u32FramePost = santos_cand3b_read_frame_counter(sCand3bState.pvVdcRegs);

	/* 4. Target retire frame: the PREVIOUS scanout buffer is retired at the first
	 *    hardware frame boundary after this successor write (golden 3.4 analogue:
	 *    successor programmed -> first following vblank -> PREVIOUS complete).
	 *    F_post + 1 is the CURRENT CONSERVATIVE SAFETY BOUNDARY, not a proven minimum;
	 *    it is kept unchanged in r8 (see the file header). */
	u32TargetRetire = (u32FramePost + 1) & 0x00ffffffu;

	/* 5. Check if any previously queued buffers have reached their target frame */
	santos_cand3b_retire_check_locked(u32FramePost, ahToComplete, &nComplete,
					  (int)ARRAY_SIZE(ahToComplete));

	/* 6. Enqueue the PREVIOUS scanout buffer for retirement at F_post + 1 */
	if (sCand3bState.psCurrentBuf && sCand3bState.hCurrentCookie)
	{
		psPrevBuf = sCand3bState.psCurrentBuf;
		hPrevCookie = sCand3bState.hCurrentCookie;
		santos_cand3b_retire_enqueue_locked(hPrevCookie, psPrevBuf, u32TargetRetire);
	}

	/* 7. Track the NEW buffer as CURRENT. It is NOT in the retirement queue!
	 * CURRENT must remain pinned and uncompleted until a successor replaces it.
	 */
	sCand3bState.psCurrentBuf = psNewBuf;
	sCand3bState.hCurrentCookie = hCmdCookie;
	sCand3bState.ui32FlipsPresented++;

	/* 8. If there are pending previous buffers in the queue, queue the bounded
	 *    frame-counter observer (single ordered work item; coalesced by the workqueue). */
	for (i = 0; i < SANTOS_CAND3B_RETIRE_QUEUE_SIZE; i++)
	{
		if (sCand3bState.asRetireQueue[i].bValid)
		{
			bHavePendingRetire = true;
			break;
		}
	}

	if (bHavePendingRetire)
	{
		santos_cand3b_schedule_retire();
	}
	spin_unlock_irqrestore(&sCand3bState.sLock, flags);

	/* 9. Complete retired cookies OUTSIDE the spinlock.  This runs inline inside the
	 *    active ProcessFlip()/Services queue processing, so IMG_FALSE is the correct
	 *    scheduling context; the async retirement worker is the only IMG_TRUE site. */
	for (i = 0; i < nComplete; i++)
	{
		if (ahToComplete[i])
		{
			psDevInfo->sPVRJTable.pfnPVRSRVCmdComplete(ahToComplete[i], IMG_FALSE);
		}
	}

	if (sCand3bState.ui32FlipsPresented <= 5 || (sCand3bState.ui32FlipsPresented % 1000) == 0)
	{
		pr_info("santos-cand3b[%u]: current=0x%08x (pte=%u) prev_target=%u f_post=%u (ret_imm=%u ret_delayed=%u)\n",
			sCand3bState.ui32FlipsPresented, psNewBuf->ui32GttApertureOffset,
			psNewBuf->ui32GttPteStart, u32TargetRetire, u32FramePost,
			sCand3bState.ui32FlipsRetiredImmediate, sCand3bState.ui32FlipsRetiredDelayed);
	}

	mutex_unlock(&sCand3bOpMutex);
	return IMG_TRUE;
}
#endif /* SANTOS10_DC_NOHW_STANDALONE */

static PVRSRV_ERROR DestroyDCSwapChain(IMG_HANDLE hDevice,
                                       IMG_HANDLE hSwapChain)
{
	DC_NOHW_DEVINFO	*psDevInfo;
	DC_NOHW_SWAPCHAIN *psSwapChain;

	/* check parameters */
	if(!hDevice
	|| !hSwapChain)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (DC_NOHW_DEVINFO*)hDevice;
	psSwapChain = (DC_NOHW_SWAPCHAIN*)hSwapChain;

#if defined(SANTOS10_DC_NOHW_STANDALONE)
	santos_cand3b_teardown();
#endif
	/* L58: A151f cache entries follow their source BM context lifetime and
	 * are released by the context-destroy callback, not by swapchain
	 * destruction (shared/query swapchain references may not destroy the
	 * DC swapchain at all). */

	/* free resources */
	if (psSwapChain->psBuffer)
	{
		FreeKernelMem(psSwapChain->psBuffer);
	}
	FreeKernelMem(psSwapChain);

	/* mark swapchain as not existing */
	psDevInfo->psSwapChain = 0;

	/* INTEGRATION_POINT: disable Vsync ISR */

	return (PVRSRV_OK);
}


static PVRSRV_ERROR SetDCDstRect(IMG_HANDLE	hDevice,
                                 IMG_HANDLE	hSwapChain,
                                 IMG_RECT	*psRect)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(psRect);

	return (PVRSRV_ERROR_NOT_SUPPORTED);
}


static PVRSRV_ERROR SetDCSrcRect(IMG_HANDLE	hDevice,
                                 IMG_HANDLE	hSwapChain,
                                 IMG_RECT	*psRect)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(psRect);

	return (PVRSRV_ERROR_NOT_SUPPORTED);
}


static PVRSRV_ERROR SetDCDstColourKey(IMG_HANDLE	hDevice,
                                      IMG_HANDLE	hSwapChain,
                                      IMG_UINT32	ui32CKColour)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(ui32CKColour);

	return (PVRSRV_ERROR_NOT_SUPPORTED);
}


static PVRSRV_ERROR SetDCSrcColourKey(IMG_HANDLE	hDevice,
                                      IMG_HANDLE	hSwapChain,
                                      IMG_UINT32	ui32CKColour)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(ui32CKColour);

	return (PVRSRV_ERROR_NOT_SUPPORTED);
}


static PVRSRV_ERROR GetDCBuffers(IMG_HANDLE hDevice,
                                 IMG_HANDLE hSwapChain,
                                 IMG_UINT32 *pui32BufferCount,
                                 IMG_HANDLE *phBuffer)
{
/*	DC_NOHW_DEVINFO	*psDevInfo; */
	DC_NOHW_SWAPCHAIN *psSwapChain;
	unsigned long i;

	/* check parameters */
	if(!hDevice
	|| !hSwapChain
	|| !pui32BufferCount
	|| !phBuffer)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

/*	psDevInfo = (DC_NOHW_DEVINFO*)hDevice; */
	psSwapChain = (DC_NOHW_SWAPCHAIN*)hSwapChain;

	/* return the buffer count */
	*pui32BufferCount = (IMG_UINT32)psSwapChain->ulBufferCount;

	/* return the buffers */
	for(i=0; i<psSwapChain->ulBufferCount; i++)
	{
		phBuffer[i] = (IMG_HANDLE)&psSwapChain->psBuffer[i];
	}

	return (PVRSRV_OK);
}


static PVRSRV_ERROR SwapToDCBuffer(IMG_HANDLE	hDevice,
                                   IMG_HANDLE	hBuffer,
                                   IMG_UINT32	ui32SwapInterval,
                                   IMG_HANDLE	hPrivateTag,
                                   IMG_UINT32	ui32ClipRectCount,
                                   IMG_RECT		*psClipRect)
{
	UNREFERENCED_PARAMETER(ui32SwapInterval);
	UNREFERENCED_PARAMETER(hPrivateTag);
	UNREFERENCED_PARAMETER(psClipRect);

	if(!hDevice
	|| !hBuffer
	|| (ui32ClipRectCount != 0))
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	/* nothing to do for no hw */
	return (PVRSRV_OK);
}


static DC_ERROR Flip(DC_NOHW_DEVINFO	*psDevInfo,
                     DC_NOHW_BUFFER		*psBuffer)
{
	UNREFERENCED_PARAMETER(psBuffer);
	/* check parameters */
	if(!psDevInfo)
	{
		return (DC_ERROR_INVALID_PARAMS);
	}
	/* to be implemented */

	return (DC_OK);
}


static IMG_BOOL ProcessFlip(IMG_HANDLE	hCmdCookie,
                            IMG_UINT32	ui32DataSize,
                            IMG_VOID	*pvData,
                            IMG_BOOL	bFlush)
{
	DC_ERROR eError;
	DISPLAYCLASS_FLIP_COMMAND *psFlipCmd;
	DC_NOHW_DEVINFO	*psDevInfo;
	DC_NOHW_BUFFER	*psBuffer;
	
#if defined(SANTOS10_DC_NOHW_STANDALONE)
	IMG_BOOL	bShuttingDown;
	unsigned long flags;
	enum santos_a151f_copy_result eCopyRes;
	IMG_BOOL	bRet;
#endif

	/* check parameters */
	if(!hCmdCookie)
	{
		return (IMG_FALSE);
	}

	/* validate data packet */
	psFlipCmd = (DISPLAYCLASS_FLIP_COMMAND*)pvData;
	if (psFlipCmd == IMG_NULL)
	{
		return (IMG_FALSE);
	}

	/* setup some useful pointers */
	psDevInfo = (DC_NOHW_DEVINFO*)psFlipCmd->hExtDevice;

	psBuffer = (DC_NOHW_BUFFER*)psFlipCmd->hExtBuffer;

	/* flip the display */
	eError = Flip(psDevInfo, psBuffer);
	if(eError != DC_OK)
	{
		return (IMG_FALSE);
	}

	/* If queue is being flushed, complete cookie and pop command */
	if (bFlush)
	{
		psDevInfo->sPVRJTable.pfnPVRSRVCmdCompleteStatus(hCmdCookie, IMG_FALSE, PVRSRV_COMMAND_STATUS_ABORTED);
		return (IMG_TRUE);
	}

#if defined(SANTOS10_DC_NOHW_STANDALONE)
	/* 1. Track in-flight callbacks and fast-drain on shutdown */
	spin_lock_irqsave(&sA151fLock, flags);
	bShuttingDown = gbA151fShuttingDown;
	if (bShuttingDown)
	{
		spin_unlock_irqrestore(&sA151fLock, flags);
		gui32A151fDrainedFlips++;
		psDevInfo->sPVRJTable.pfnPVRSRVCmdCompleteStatus(hCmdCookie, IMG_FALSE, PVRSRV_COMMAND_STATUS_ABORTED);
		return (IMG_TRUE);
	}
	gui32A151fInFlightFlips++;
	spin_unlock_irqrestore(&sA151fLock, flags);
#endif

	/* Candidate 3B zero-copy scanout or A151f CPU-copy */
	if (ui32DataSize == sizeof(DISPLAYCLASS_FLIP_COMMAND2))
	{
#if defined(SANTOS10_DC_NOHW_STANDALONE)
		if (santos_cand3b)
		{
			if (santos_cand3b_try_flip(psDevInfo,
						   (DISPLAYCLASS_FLIP_COMMAND2 *)pvData,
						   hCmdCookie))
			{
				spin_lock_irqsave(&sA151fLock, flags);
				gui32A151fInFlightFlips--;
				spin_unlock_irqrestore(&sA151fLock, flags);
				return (IMG_TRUE);
			}
			/* If Candidate 3B failed, fall through to baseline A151f copy */
		}

		eCopyRes = santos_a151f_try_copy(psDevInfo,
						 (DISPLAYCLASS_FLIP_COMMAND2 *)pvData);
		switch (eCopyRes)
		{
		case SANTOS_A151F_COPY_SUCCESS:
			/* Cycle 4: gui32A151fCopies is incremented exactly once, inside
			 * santos_a151f_try_copy() under sA151fOpMutex (together with the
			 * heartbeat accounting). Do not increment again here. */
			psDevInfo->sPVRJTable.pfnPVRSRVCmdCompleteStatus(hCmdCookie, IMG_FALSE,
															 PVRSRV_COMMAND_STATUS_PRESENTED);
			bRet = IMG_TRUE;
			break;

		case SANTOS_A151F_COPY_RETRY:
			bRet = IMG_FALSE;
			break;

		case SANTOS_A151F_COPY_PERM_FAIL:
		default:
			gui32A151fFailedCopies++;
			/* Cycle 4: a failed presentation is queue-complete (syncs
			 * advanced, queue popped, meminfo released) so the queue never
			 * wedges, but the frame was NOT presented: the display keeps
			 * showing the previous frame and the stored completion status
			 * stays FAILED (never remapped to PRESENTED). The Services
			 * 1.x queue has no fence/userspace failure channel (see
			 * queue.h), so make every failure observable here instead of
			 * counting it silently. */
			pr_err_ratelimited("santos-a151f: presentation FAILED (total failed %u, presented %u); keeping previous frame\n",
					   gui32A151fFailedCopies, gui32A151fCopies);
			psDevInfo->sPVRJTable.pfnPVRSRVCmdCompleteStatus(hCmdCookie, IMG_FALSE,
															 PVRSRV_COMMAND_STATUS_FAILED);
			bRet = IMG_TRUE;
			break;
		}

		spin_lock_irqsave(&sA151fLock, flags);
		gui32A151fInFlightFlips--;
		spin_unlock_irqrestore(&sA151fLock, flags);
		return bRet;
#else
		psDevInfo->sPVRJTable.pfnPVRSRVCmdCompleteStatus(hCmdCookie, IMG_FALSE,
														 PVRSRV_COMMAND_STATUS_PRESENTED);
		return (IMG_TRUE);
#endif
	}

	/* call command complete Callback for non-COMMAND2 legacy dummy flip */
	psDevInfo->sPVRJTable.pfnPVRSRVCmdCompleteStatus(hCmdCookie, IMG_FALSE,
													 PVRSRV_COMMAND_STATUS_PRESENTED);

#if defined(SANTOS10_DC_NOHW_STANDALONE)
	spin_lock_irqsave(&sA151fLock, flags);
	gui32A151fInFlightFlips--;
	spin_unlock_irqrestore(&sA151fLock, flags);
#endif

	return (IMG_TRUE);
}


DC_ERROR Init(void)
{
	DC_NOHW_DEVINFO *psDevInfo;
	DC_ERROR         eError;
	unsigned long    ulBBuf;
	unsigned long    ulNBBuf;
	/*
		- connect to services
		- register with services
		- allocate and setup private data structure
	*/


	/*
		in kernel driver, data structures must be anchored to something for subsequent retrieval
		this may be a single global pointer or TLS or something else - up to you
		call API to retrieve this ptr
	*/

	/*
		get the anchor pointer
	*/
	psDevInfo = GetAnchorPtr();

	if (psDevInfo == 0)
	{
		PFN_CMD_PROC  pfnCmdProcList[DC_NOHW_COMMAND_COUNT];
		IMG_UINT32    aui32SyncCountList[DC_NOHW_COMMAND_COUNT][2];

		/* allocate device info. structure */
		psDevInfo = (DC_NOHW_DEVINFO *)AllocKernelMem(sizeof(*psDevInfo));

		if(!psDevInfo)
		{
			eError = DC_ERROR_OUT_OF_MEMORY;/* failure */
			goto ExitError;
		}

		/* initialise allocation */
		memset(psDevInfo, 0, sizeof(*psDevInfo));

		/* set the top-level anchor */
		SetAnchorPtr((void*)psDevInfo);

		/* set ref count */
		psDevInfo->ulRefCount = 0UL;

#if defined(SANTOS10_DC_NOHW_STANDALONE)
		pr_info("santos-dc: devinfo allocated\n");
#endif

		if(OpenPVRServices(&psDevInfo->hPVRServices) != DC_OK)
		{
			eError = DC_ERROR_INIT_FAILURE;
			goto ExitFreeDevInfo;
		}
		if(GetLibFuncAddr (psDevInfo->hPVRServices, "PVRGetDisplayClassJTable", &pfnGetPVRJTable) != DC_OK)
		{
			eError = DC_ERROR_INIT_FAILURE;
			goto ExitCloseServices;
		}

		/* got the kernel services function table */
		if((*pfnGetPVRJTable)(&psDevInfo->sPVRJTable) == IMG_FALSE)
		{
			eError = DC_ERROR_INIT_FAILURE;
			goto ExitCloseServices;
		}

#if defined(SANTOS10_DC_NOHW_STANDALONE)
		pr_info("santos-dc: services jtable ok\n");
#endif

		/*
			Setup the devinfo
		*/
		psDevInfo->psSwapChain = 0;
		psDevInfo->sDisplayInfo.ui32MinSwapInterval = 0UL;
		psDevInfo->sDisplayInfo.ui32MaxSwapInterval = 1UL;
		psDevInfo->sDisplayInfo.ui32MaxSwapChains = 1UL;
		psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers = DC_NOHW_MAX_BACKBUFFERS;
		strscpy(psDevInfo->sDisplayInfo.szDisplayName, DISPLAY_DEVICE_NAME, MAX_DISPLAY_NAME_SIZE);

		psDevInfo->ulNumFormats = 1UL;

		psDevInfo->ulNumDims = 1UL;

#if defined(DC_NOHW_GET_BUFFER_DIMENSIONS)
		if (!GetBufferDimensions(&psDevInfo->asDisplayDimList[0].ui32Width,
			&psDevInfo->asDisplayDimList[0].ui32Height,
			&psDevInfo->asDisplayFormatList[0].pixelformat,
			&psDevInfo->asDisplayDimList[0].ui32ByteStride))
		{
			eError = DC_ERROR_INIT_FAILURE;
			goto ExitCloseServices;
		}
#else	/* defined(DC_NOHW_GET_BUFFER_DIMENSIONS) */
	#if defined (ENABLE_DISPLAY_MODE_TRACKING)
		// Set sizes to zero to force re-alloc on display mode change.
		psDevInfo->asDisplayFormatList[0].pixelformat = DC_NOHW_BUFFER_PIXEL_FORMAT;
		psDevInfo->asDisplayDimList[0].ui32Width =  0;
		psDevInfo->asDisplayDimList[0].ui32Height =  0;
		psDevInfo->asDisplayDimList[0].ui32ByteStride = 0;
	#else
		psDevInfo->asDisplayFormatList[0].pixelformat = DC_NOHW_BUFFER_PIXEL_FORMAT;
		psDevInfo->asDisplayDimList[0].ui32Width =  DC_NOHW_BUFFER_WIDTH;
		psDevInfo->asDisplayDimList[0].ui32Height =  DC_NOHW_BUFFER_HEIGHT;
		psDevInfo->asDisplayDimList[0].ui32ByteStride = DC_NOHW_BUFFER_BYTE_STRIDE;
	#endif
#endif	/* defined(DC_NOHW_GET_BUFFER_DIMENSIONS) */

		psDevInfo->sSysFormat = psDevInfo->asDisplayFormatList[0];
		psDevInfo->sSysDims.ui32Width = psDevInfo->asDisplayDimList[0].ui32Width;
		psDevInfo->sSysDims.ui32Height = psDevInfo->asDisplayDimList[0].ui32Height;
		psDevInfo->sSysDims.ui32ByteStride = psDevInfo->asDisplayDimList[0].ui32ByteStride;
		psDevInfo->ui32BufferSize = psDevInfo->sSysDims.ui32Height * psDevInfo->sSysDims.ui32ByteStride;


		/* setup swapchain details */
		for(ulBBuf=0; ulBBuf<DC_NOHW_MAX_BACKBUFFERS; ulBBuf++)
		{
#if defined(USE_BASE_VIDEO_FRAMEBUFFER) || defined (ENABLE_DISPLAY_MODE_TRACKING)
			psDevInfo->asBackBuffers[ulBBuf].sSysAddr.uiAddr = IMG_NULL;
			psDevInfo->asBackBuffers[ulBBuf].sCPUVAddr = IMG_NULL;
#else
#if defined(DC_NOHW_DISCONTIG_BUFFERS)
			if (AllocDiscontigMemory(psDevInfo->ui32BufferSize,
								  &psDevInfo->asBackBuffers[ulBBuf].hMemChunk,
								  &psDevInfo->asBackBuffers[ulBBuf].sCPUVAddr,
								  &psDevInfo->asBackBuffers[ulBBuf].psSysAddr) != DC_OK)
			{
				eError = DC_ERROR_INIT_FAILURE;
				goto ExitFreeMem;
			}
#else
			IMG_CPU_PHYADDR		sBufferCPUPAddr;

			if (AllocContigMemory(psDevInfo->ui32BufferSize,
								  &psDevInfo->asBackBuffers[ulBBuf].hMemChunk,
								  &psDevInfo->asBackBuffers[ulBBuf].sCPUVAddr,
								  &sBufferCPUPAddr) != DC_OK)
			{
				eError = DC_ERROR_INIT_FAILURE;
				goto ExitFreeMem;
			}

			psDevInfo->asBackBuffers[ulBBuf].sSysAddr =  CpuPAddrToSysPAddr(sBufferCPUPAddr);
#endif
#endif /* #if defined(USE_BASE_VIDEO_FRAMEBUFFER) */
			/* sDevVAddr not meaningful for nohw */
			psDevInfo->asBackBuffers[ulBBuf].sDevVAddr.uiAddr = 0UL;
			psDevInfo->asBackBuffers[ulBBuf].hSwapChain = 0;
			psDevInfo->asBackBuffers[ulBBuf].psSyncData = 0;
			psDevInfo->asBackBuffers[ulBBuf].psNext = 0;
		}

#if defined(SANTOS10_DC_NOHW_STANDALONE)
		pr_info("santos-dc: back buffers ok\n");
#endif

		/*
			setup the DC Jtable so SRVKM can call into this driver
		*/
		psDevInfo->sDCJTable.ui32TableSize = sizeof(PVRSRV_DC_SRV2DISP_KMJTABLE);
		psDevInfo->sDCJTable.pfnOpenDCDevice = OpenDCDevice;
		psDevInfo->sDCJTable.pfnCloseDCDevice = CloseDCDevice;
		psDevInfo->sDCJTable.pfnEnumDCFormats = EnumDCFormats;
		psDevInfo->sDCJTable.pfnEnumDCDims = EnumDCDims;
		psDevInfo->sDCJTable.pfnGetDCSystemBuffer = GetDCSystemBuffer;
		psDevInfo->sDCJTable.pfnGetDCInfo = GetDCInfo;
		psDevInfo->sDCJTable.pfnGetBufferAddr = GetDCBufferAddr;
		psDevInfo->sDCJTable.pfnCreateDCSwapChain = CreateDCSwapChain;
		psDevInfo->sDCJTable.pfnDestroyDCSwapChain = DestroyDCSwapChain;
		psDevInfo->sDCJTable.pfnSetDCDstRect = SetDCDstRect;
		psDevInfo->sDCJTable.pfnSetDCSrcRect = SetDCSrcRect;
		psDevInfo->sDCJTable.pfnSetDCDstColourKey = SetDCDstColourKey;
		psDevInfo->sDCJTable.pfnSetDCSrcColourKey = SetDCSrcColourKey;
		psDevInfo->sDCJTable.pfnGetDCBuffers = GetDCBuffers;
		psDevInfo->sDCJTable.pfnSwapToDCBuffer = SwapToDCBuffer;
		psDevInfo->sDCJTable.pfnSetDCState = IMG_NULL;

		/* register device with services and retrieve device index */
		if(psDevInfo->sPVRJTable.pfnPVRSRVRegisterDCDevice (&psDevInfo->sDCJTable,
															&psDevInfo->uiDeviceID ) != PVRSRV_OK)
		{
			eError = DC_ERROR_DEVICE_REGISTER_FAILED;
			goto ExitFreeMem;
		}

#if defined(SANTOS10_DC_NOHW_STANDALONE)
		pr_info("santos-dc: registered id=%u\n", psDevInfo->uiDeviceID);
		if (santos_gtt_census && !gbSantosGttCensusDone)
		{
			santos_gtt_census_probe();
		}
		if (santos_gtt_preflight && !gbSantosGttPreflightDone)
		{
			santos_gtt_preflight_probe();
		}
#endif

		/*
			setup private command processing function table
		*/
		pfnCmdProcList[DC_FLIP_COMMAND] = ProcessFlip;

		/*
			and associated sync count(s)
		*/
		aui32SyncCountList[DC_FLIP_COMMAND][0] = 0UL;/* no writes */
		aui32SyncCountList[DC_FLIP_COMMAND][1] = 2UL;/* 2 reads: To / From */

		/*
			register private command processing functions with
			the Command Queue Manager and setup the general
			command complete function in the devinfo
		*/
		if (psDevInfo->sPVRJTable.pfnPVRSRVRegisterCmdProcList(psDevInfo->uiDeviceID,
															   &pfnCmdProcList[0],
															   aui32SyncCountList,
															   DC_NOHW_COMMAND_COUNT) != PVRSRV_OK)
		{
			eError = DC_ERROR_CANT_REGISTER_CALLBACK;
			goto ExitRemoveDevice;
		}

#if defined(SANTOS10_DC_NOHW_STANDALONE)
		pr_info("santos-dc: cmd proc registered\n");
#endif
	}

	/* increment the ref count */
	psDevInfo->ulRefCount++;

	/* return success */
	return (DC_OK);

ExitRemoveDevice:
	(IMG_VOID) psDevInfo->sPVRJTable.pfnPVRSRVRemoveDCDevice(psDevInfo->uiDeviceID);

ExitFreeMem:
	ulNBBuf = ulBBuf;
	for(ulBBuf=0; ulBBuf<ulNBBuf; ulBBuf++)
	{
#if defined(DC_NOHW_DISCONTIG_BUFFERS)
		FreeDiscontigMemory(psDevInfo->ui32BufferSize,
			 psDevInfo->asBackBuffers[ulBBuf].hMemChunk,
			 psDevInfo->asBackBuffers[ulBBuf].sCPUVAddr,
			 psDevInfo->asBackBuffers[ulBBuf].psSysAddr);
#else
#if !defined(USE_BASE_VIDEO_FRAMEBUFFER)

		FreeContigMemory(psDevInfo->ui32BufferSize,
			 psDevInfo->asBackBuffers[ulBBuf].hMemChunk,
			 psDevInfo->asBackBuffers[ulBBuf].sCPUVAddr,
			 SysPAddrToCpuPAddr(psDevInfo->asBackBuffers[ulBBuf].sSysAddr));


#endif /* #if defined(USE_BASE_VIDEO_FRAMEBUFFER) */
#endif /* #if defined(DC_NOHW_DISCONTIG_BUFFERS) */
	}

ExitCloseServices:
	(void)ClosePVRServices(psDevInfo->hPVRServices);

ExitFreeDevInfo:
	FreeKernelMem(psDevInfo);
	SetAnchorPtr(0);

ExitError:
	return eError;
}



/*
 *	Deinit
 *
 * Cycle 7 transactional lifecycle (PREPARE / COMMIT), Cycle 8 lifecycle
 * state ownership. The caller (DC_NOHW_Cleanup) holds sDCLifecycleMutex for
 * the whole transition, so Deinit must never take it itself; that is what
 * serializes Init and Deinit against each other.
 *
 * On success or failure this function leaves giDCNoHwLifecycle at the state
 * the caller must publish:
 *   - full teardown reached OFF           -> DC_OK, OFF
 *   - PREPARE rolled back, nothing lost   -> failure, RUNNING
 *   - Services node removed, resume later -> failure, STAGED
 *
 * PREPARE is reversible and retires nothing:
 *   P1  PVRSRVFreezeCmdProcKM() closes new display-swap admission while the
 *       command-processor table stays published, so commands already in the
 *       Services queue keep dispatching and completing naturally. Unlike
 *       Cycle 4/5/6 this does NOT set gbA151fShuttingDown: an RCU/MISR timed
 *       run cannot retire a queued command as ABORTED just because DeInit was
 *       attempted, so a later PREPARE failure cannot lose a command.
 *   P2  observe natural quiescence without kicking or flushing the queue:
 *       in-flight presentation callbacks == 0, no admission/reader pins, no
 *       submitted-but-undispatched commands, no command entry in use. The
 *       submitted-command ownership count is what makes this complete: a
 *       command that was submitted and its admission pin released, but not
 *       yet dispatched/popped, keeps the device busy here.
 *   P3  remove the Services device node (the only other fallible lifecycle
 *       step). Its preconditions are proven by C0 below: with no open
 *       Services DC connections (ui32RefCount == 0) the removal cannot fail
 *       (deviceclass.c: PVRSRVRemoveDCDeviceKM). A failure here still has
 *       presentation fully usable.
 *   Any PREPARE failure calls PVRSRVUnfreezeCmdProcKM() (and aborts the
 *   staged commit, if it had already been marked) returning the driver to
 *   the exact RUNNING topology it had before the attempt; no queued command
 *   was popped, no sync counter advanced, no private data released, and the
 *   A151f mappings/framebuffer remain usable.
 *
 * ---- COMMIT BOUNDARY: from here the teardown cannot be rolled back ----
 *   C1  detach the command-processor table (state closed; the table is
 *       parked, not yet freed). Detach cannot strand a queued command
 *       because P2 proved the ownership count is zero and admission stayed
 *       frozen. Detach is retried a bounded number of times; if it cannot
 *       complete, the staged marker below lets a later DeInit (module
 *       unload retry) resume here without re-running P1..P3.
 *   C2  destroy presentation mappings (void, idempotent).
 *   C3  close Services (stub: always DC_OK, dc_nohw_linux.c).
 *   C4  free the detached table (re-validated; admission frozen since P1 and
 *       all users settled, so this must pass; bounded retry is defense in
 *       depth, and a retry resumes at C3 without repeating C1/C2).
 *   C5  commit refcount and release memory.
 */
DC_ERROR Deinit(void)
{
	DC_NOHW_DEVINFO *psDevInfo, *psDevFirst;
#if !defined(USE_BASE_VIDEO_FRAMEBUFFER)
	unsigned long i;
#endif

	psDevFirst = GetAnchorPtr();
	psDevInfo = psDevFirst;

	/* check DevInfo has been setup */
	if (psDevInfo == 0)
	{
		/* Idempotent: no anchor means nothing owns the DC device. This is
		 * the normal case at module unload when DC was never enabled or a
		 * previous Cleanup already reached OFF. */
		giDCNoHwLifecycle = DC_NOHW_LIFECYCLE_OFF;
		return (DC_OK);
	}

	/* If other references remain, decrement refcount and return success */
	if (psDevInfo->ulRefCount > 1UL)
	{
		psDevInfo->ulRefCount--;
		return (DC_OK);
	}

	/* ulRefCount == 1: final teardown. */
	{
		/* all references gone - de-init device information */
		PVRSRV_DC_DISP2SRV_KMJTABLE	*psJTable = &psDevInfo->sPVRJTable;
		IMG_UINT32 ui32Retry;
		IMG_BOOL bQuiesced = IMG_FALSE;

		if (giDCNoHwLifecycle != DC_NOHW_LIFECYCLE_STAGED)
		{
			/* ---- PREPARE: reversible; may fail with state intact ---- */

			/* P1: freeze admission (no new swaps, table still live). */
			if (PVRSRVFreezeCmdProcKM(psDevInfo->uiDeviceID) != PVRSRV_OK)
			{
				pr_err("santos-dc: Deinit: cannot freeze admission, refusing partial teardown\n");
				return (DC_ERROR_GENERIC);/* failure */
			}

			/* P2: wait for natural quiescence. No queue kicks, no
			 * ABORTED-as-probe: dispatch and completion of already
			 * submitted commands proceed normally. */
			for (ui32Retry = 0; ui32Retry < SANTOS_A151F_QUIESCE_MS; ui32Retry++)
			{
				msleep(1);
				if (santos_a151f_in_flight() == 0 &&
				    PVRSRVQueryCmdProcIdleKM(psDevInfo->uiDeviceID,
							     DC_NOHW_COMMAND_COUNT))
				{
					bQuiesced = IMG_TRUE;
					break;
				}
			}
			if (!bQuiesced)
			{
				pr_err("santos-dc: Deinit: not quiesced, refusing partial teardown\n");
				(void)PVRSRVUnfreezeCmdProcKM(psDevInfo->uiDeviceID);
				return (DC_ERROR_GENERIC);/* failure */
			}

			/* P3: remove the Services device node. Preconditions are
			 * proven at this point: no open connections (removal only
			 * fails on ui32RefCount != 0), no queued commands, no
			 * in-flight callbacks. */
			if (psJTable->pfnPVRSRVRemoveDCDevice((IMG_UINT32)psDevInfo->uiDeviceID) != PVRSRV_OK)
			{
				pr_err("santos-dc: Deinit: device removal failed, restoring admission\n");
				(void)PVRSRVUnfreezeCmdProcKM(psDevInfo->uiDeviceID);
				return (DC_ERROR_GENERIC);/* failure */
			}
			giDCNoHwLifecycle = DC_NOHW_LIFECYCLE_STAGED;
		}

		/* ---- COMMIT BOUNDARY: state cannot be rolled back ---- */

		/* C1: detach the command-processor table (parked, not freed).
		 * Bounded retry: the only contention left is transient lock/pin
		 * activity from finite readers; the count/entry checks passed
		 * above and admission stays frozen. A failure here is staged
		 * (DC_NOHW_LIFECYCLE_STAGED) and a later DeInit resumes at C2
		 * without repeating the removal. */
		{
			IMG_BOOL bDetached = IMG_FALSE;

			for (ui32Retry = 0; ui32Retry < SANTOS_A151F_QUIESCE_MS; ui32Retry++)
			{
				if (PVRSRVDetachCmdProcKM(psDevInfo->uiDeviceID,
							  DC_NOHW_COMMAND_COUNT) == PVRSRV_OK)
				{
					bDetached = IMG_TRUE;
					break;
				}
				msleep(1);
			}
			if (!bDetached)
			{
				pr_err("santos-dc: Deinit: detach failed post-commit; resume with a retry\n");
				return (DC_ERROR_GENERIC);/* failure; staged: DC_NOHW_LIFECYCLE_STAGED */
			}
		}

		/* C2: tear down presentation mappings (void, idempotent on resume) */
#if defined(SANTOS10_DC_NOHW_STANDALONE)
		santos_cand3b_teardown();
#endif
		santos_a151f_teardown();

		/* C3: Close Services connection (stub: always DC_OK) */
		if (ClosePVRServices(psDevInfo->hPVRServices) != DC_OK)
		{
			psDevInfo->hPVRServices = 0;
			return (DC_ERROR_GENERIC);/* failure */
		}

		/* C4: free the detached table. Re-validation must pass: no
		 * queued commands and no dispatch are possible since C1. */
		for (ui32Retry = 0; ; ui32Retry++)
		{
			if (PVRSRVFreeDetachedCmdProcKM(psDevInfo->uiDeviceID,
							DC_NOHW_COMMAND_COUNT) == PVRSRV_OK)
			{
				break;
			}
			if (ui32Retry >= SANTOS_A151F_QUIESCE_MS)
			{
				pr_err("santos-dc: Deinit: detached-table free failed post-commit; resume with a retry\n");
				return (DC_ERROR_GENERIC);/* failure; staged: DC_NOHW_LIFECYCLE_STAGED */
			}
			msleep(1);
		}

		/* C5: Commit final de-init */
		psDevInfo->ulRefCount = 0UL;
		giDCNoHwLifecycle = DC_NOHW_LIFECYCLE_OFF;

#if !defined(USE_BASE_VIDEO_FRAMEBUFFER)
		for(i=0; i<DC_NOHW_MAX_BACKBUFFERS; i++)
		{
			if (psDevInfo->asBackBuffers[i].sCPUVAddr)
			{
				#if defined(DC_NOHW_DISCONTIG_BUFFERS)
				FreeDiscontigMemory(psDevInfo->ui32BufferSize,
							 psDevInfo->asBackBuffers[i].hMemChunk,
							 psDevInfo->asBackBuffers[i].sCPUVAddr,
							 psDevInfo->asBackBuffers[i].psSysAddr);
				#else

				FreeContigMemory(psDevInfo->ui32BufferSize,
							 psDevInfo->asBackBuffers[i].hMemChunk,
							 psDevInfo->asBackBuffers[i].sCPUVAddr,
							 SysPAddrToCpuPAddr(psDevInfo->asBackBuffers[i].sSysAddr));
				#endif
			}
		}
#endif /* #if !defined(USE_BASE_VIDEO_FRAMEBUFFER) */

		/* de-allocate data structure */
		FreeKernelMem(psDevInfo);
	}

#if defined (ENABLE_DISPLAY_MODE_TRACKING)
	CloseMiniport();
#endif
	/* clear the top-level anchor */
	SetAnchorPtr(0);

	/* return success */
	return (DC_OK);
}


#if defined(SANTOS10_DC_NOHW_STANDALONE)
/*
 * Cycle 8 public DC lifecycle API. See dc_nohw.h for the state contract.
 */
DC_ERROR DC_NOHW_Init(void)
{
	DC_ERROR eError;

	mutex_lock(&sDCLifecycleMutex);

	switch (giDCNoHwLifecycle)
	{
	case DC_NOHW_LIFECYCLE_OFF:
		/*
		 * Cycle 9: acquire module lifetime ownership BEFORE any OFF->RUNNING
		 * mutation. delete_module()/try_stop_module() drops the loader's
		 * base reference and marks the module MODULE_STATE_GOING once the
		 * refcount reaches zero (kernel/module/main.c); try_module_get()
		 * then fails via module_is_live(). Taking the pin first makes the
		 * two outcomes atomic and safe:
		 *
		 *  - pin acquired: the extra reference makes try_release_module_ref()
		 *    fail, so rmmod returns -EBUSY and module_exit (and therefore
		 *    SysDeinitialise/PVRSyncDeviceDeInit/santos_pvr_irq_stop) cannot
		 *    start while Init() builds the DC topology;
		 *  - pin refused (module already GOING): Init() is never called and
		 *    no DC/Services/cmdproc state is touched.
		 *
		 * During module load the module is MODULE_STATE_COMING, which is
		 * still live for try_module_get(), so the boot-time enable
		 * (santos_dc=1) keeps working; a load failure before this point
		 * never reaches here, and a failure after it cannot occur in this
		 * configuration (SysInitialise's DC init is its last step).
		 */
		if (!try_module_get(THIS_MODULE))
		{
			pr_err("santos-dc: module is going away, enable refused\n");
			giDCNoHwLifecycle = DC_NOHW_LIFECYCLE_OFF;
			eError = DC_ERROR_GENERIC;
			break;
		}
		eError = Init();
		if (eError != DC_OK)
		{
			/* Init() is self-unwinding (unregisters DC device/cmdproc and
			 * frees the anchor on every failure exit). Release the early
			 * pin exactly once and remain OFF. */
			module_put(THIS_MODULE);
			giDCNoHwLifecycle = DC_NOHW_LIFECYCLE_OFF;
			break;
		}
		/* Retain the same reference taken above as the RUNNING/STAGED
		 * lifetime pin; never acquire a second one. */
		gbDCModulePinHeld = IMG_TRUE;
		giDCNoHwLifecycle = DC_NOHW_LIFECYCLE_RUNNING;
		pr_info("santos-dc: lifecycle OFF -> RUNNING (module pin held)\n");
		break;

	case DC_NOHW_LIFECYCLE_RUNNING:
		/* Idempotent enable edge: no extra reference and no extra pin. */
		eError = DC_OK;
		break;

	case DC_NOHW_LIFECYCLE_STAGED:
	default:
		pr_err("santos-dc: init refused, previous teardown is staged\n");
		eError = DC_ERROR_GENERIC;
		break;
	}

	mutex_unlock(&sDCLifecycleMutex);
	return eError;
}

DC_ERROR DC_NOHW_Cleanup(void)
{
	DC_ERROR eError = DC_OK;

	mutex_lock(&sDCLifecycleMutex);

	switch (giDCNoHwLifecycle)
	{
	case DC_NOHW_LIFECYCLE_OFF:
		/* Idempotent: nothing to clean up, nothing pinned. */
		eError = DC_OK;
		break;

	case DC_NOHW_LIFECYCLE_RUNNING:
	case DC_NOHW_LIFECYCLE_STAGED:
		eError = Deinit();
		if (eError == DC_OK && giDCNoHwLifecycle == DC_NOHW_LIFECYCLE_OFF)
		{
			if (gbDCModulePinHeld)
			{
				gbDCModulePinHeld = IMG_FALSE;
				module_put(THIS_MODULE);
				pr_info("santos-dc: lifecycle -> OFF (module pin released)\n");
			}
		}
		break;
	}

	mutex_unlock(&sDCLifecycleMutex);
	return eError;
}

int DC_NOHW_GetLifecycle(void)
{
	int iState;

	mutex_lock(&sDCLifecycleMutex);
	iState = giDCNoHwLifecycle;
	mutex_unlock(&sDCLifecycleMutex);
	return iState;
}
#endif /* defined(SANTOS10_DC_NOHW_STANDALONE) */

/******************************************************************************
 End of file (dc_nohw_displayclass.c)
******************************************************************************/
