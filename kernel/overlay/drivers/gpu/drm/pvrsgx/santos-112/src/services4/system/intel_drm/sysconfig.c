
/**********************************************************************
 *
 * Copyright (C) Imagination Technologies Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful but, except
 * as otherwise stated in writing, without any warranty; without even the
 * implied warranty of merchantability or fitness for a particular purpose.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK
 *
 ******************************************************************************/

#if defined(LDM_PCI) || defined(SUPPORT_DRI_DRM)
#include "linux/pci.h"
#endif
#if defined(SUPPORT_DRI_DRM)
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#endif

#include "sgxdefs.h"
#include "services_headers.h"
#include "kerneldisplay.h"
#include "oemfuncs.h"
#include "sgxinfo.h"
#include "sgxinfokm.h"
#include "pdump_km.h"
#include "syslocal.h"
#include "mdfld_gl3.h"
#if defined(SUPPORT_DRI_DRM_EXT)
#include "psb_drv.h"
#include "psb_powermgmt.h"
#include "sys_pvr_drm_export.h"
#include "santos-irq.h"

#endif

extern struct drm_device *gpDrmDevice;

#if defined(SANTOS10_SGX_DC_NOHW)
extern int DC_NOHW_Init(void);
extern int DC_NOHW_Cleanup(void);
extern int DC_NOHW_ProcInit(void);
extern void DC_NOHW_ProcArm(void);
extern void DC_NOHW_ProcExit(void);
extern int g_santos10_dc_nohw_enable;
#endif

#ifndef IS_MDFLD
#define IS_MDFLD(x) (0)
#endif

//#define SYS_SGX_CLOCK_SPEED			(400000000)
#define SYS_SGX_HWRECOVERY_TIMEOUT_FREQ		(100)
#define SYS_SGX_PDS_TIMER_FREQ			(1000)
#ifdef CONFIG_GFX_ON_GI
#define SYS_SGX_ACTIVE_POWER_LATENCY_MS                (50)
#else
#define SYS_SGX_ACTIVE_POWER_LATENCY_MS                (2)
#endif

#if defined(SUPPORT_DRI_DRM_EXT)
#define	DRI_DRM_STATIC
#else
#define	DRI_DRM_STATIC	static
#endif

SYS_DATA* gpsSysData = (SYS_DATA*)IMG_NULL;
SYS_DATA  gsSysData;

static SYS_SPECIFIC_DATA gsSysSpecificData;

#if defined(DRM_PVR_USE_INTEL_FB)
IMG_UINT32 gui32SGXDeviceID;
#else
static IMG_UINT32 gui32SGXDeviceID;
#endif
#if defined(PVR_MDFLD_SYS_MSVDX_AND_TOPAZ)
IMG_UINT32		gui32MRSTMSVDXDeviceID;
IMG_UINT32		gui32MRSTTOPAZDeviceID;
#endif

static SGX_DEVICE_MAP	gsSGXDeviceMap;

#if defined(SUPPORT_DRI_DRM_EXT)
static PVRSRV_DEVICE_NODE *gpsSGXDevNode;
#endif

#if !defined(NO_HARDWARE)
IMG_CPU_VIRTADDR gsPoulsboRegsCPUVaddr;

IMG_CPU_VIRTADDR gsPoulsboDisplayRegsCPUVaddr;
#endif

#if defined(SUPPORT_DRI_DRM)
extern struct drm_device *gpsPVRDRMDev;
#endif

#if defined(LDM_PCI) || defined(SUPPORT_DRI_DRM)
extern struct pci_dev *gpsPVRLDMDev;

#if defined(SANTOS10_SGX_EARLY_WA)
/*
 * Santos (Cloverview) early SGX access workaround.
 *
 * Golden Samsung 3.4 psb_driver_load() programs the SGX544MP master clock
 * gating (sgx_reg + 0x4000/+0x4004/+0x4020) before the first SGX register
 * access. On the 7.2 port the same four writes must precede any SGX MMIO or
 * the SoC bus locks up (A104/A121 read-first hangs vs A123 WA-first success).
 * Offsets are relative to the mapped SGX register base
 * (BAR0 + MRST_SGX_REGS_OFFSET = +0x80000). The writes are posted and
 * idempotent; gated on the 0x08c8 device id (golden IS_CTP_NEED_WA).
 */
IMG_VOID SysSGXEarlyWA(IMG_VOID *pvSGXRegs)
{
	if (pvSGXRegs == IMG_NULL)
		return;

	if (gpsPVRLDMDev == IMG_NULL || gpsPVRLDMDev->device != 0x08c8)
		return;

	OSWriteHWReg(pvSGXRegs, 0x4000, 0x1);
	OSWriteHWReg(pvSGXRegs, 0x4004, 0x5);
	OSWriteHWReg(pvSGXRegs, 0x4004, 0xa);
	OSWriteHWReg(pvSGXRegs, 0x4020, 0x2aa);

	printk(KERN_WARNING "SysSGXEarlyWA: golden pre-access SGX clock-gating programmed (4 writes)\n");
}
#endif /* SANTOS10_SGX_EARLY_WA */
#endif

#if !defined(SUPPORT_DRI_DRM_EXT)
IMG_UINT32 PVRSRV_BridgeDispatchKM( IMG_UINT32  Ioctl,
									IMG_BYTE   *pInBuf,
									IMG_UINT32  InBufLen,
									IMG_BYTE   *pOutBuf,
									IMG_UINT32  OutBufLen,
									IMG_UINT32 *pdwBytesTransferred);
#endif

#define	POULSBO_ADDR_RANGE_INDEX	(MMADR_INDEX - 4)
#define	POULSBO_HP_ADDR_RANGE_INDEX	(GMADR_INDEX - 4)
static PVRSRV_ERROR PCIInitDev(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;
	IMG_UINT32 ui32MaxOffset = MRST_MAX_OFFSET;

#if defined(SUPPORT_DRI_DRM_EXT)
	if (!IS_MDFLD(psSysSpecData->psDRMDev) && !IS_MRST(psSysSpecData->psDRMDev) && !IS_POULSBO(psSysSpecData->psDRMDev))
	{
		PVR_DPF((PVR_DBG_ERROR,"PCIInitDev: Device not supported"));
		return PVRSRV_ERROR_NOT_SUPPORTED;
	}

	/* DMA must not depend on the bootloader leaving BusMaster enabled.
	 * MSI remains owned exclusively by the standalone shell. */
	psSysSpecData->hSGXPCI = OSPCISetDev((IMG_VOID *)psSysSpecData->psPCIDev,
					 HOST_PCI_INIT_FLAG_BUS_MASTER);
	ui32MaxOffset = (IS_MRST(psSysSpecData->psDRMDev) || IS_MDFLD(psSysSpecData->psDRMDev)) ? MRST_MAX_OFFSET : POULSBO_MAX_OFFSET;

#else
#if defined(LDM_PCI) || defined(SUPPORT_DRI_DRM)
	psSysSpecData->hSGXPCI = OSPCISetDev((IMG_VOID *)psSysSpecData->psPCIDev, HOST_PCI_INIT_FLAG_BUS_MASTER | HOST_PCI_INIT_FLAG_MSI);
#else
	psSysSpecData->hSGXPCI = OSPCIAcquireDev(SYS_SGX_DEV_VENDOR_ID, SYS_SGX_DEV_DEVICE_ID, HOST_PCI_INIT_FLAG_BUS_MASTER | HOST_PCI_INIT_FLAG_MSI);
#endif
#endif
	if (!psSysSpecData->hSGXPCI)
	{
		PVR_DPF((PVR_DBG_ERROR,"PCIInitDev: Failed to acquire PCI device"));
		return PVRSRV_ERROR_PCI_DEVICE_NOT_FOUND;
	}

	 SYS_SPECIFIC_DATA_SET(psSysSpecData, SYS_SPECIFIC_DATA_PCI_ACQUIRE_DEV);

	PVR_TRACE(("PCI memory region: %x to %x", OSPCIAddrRangeStart(psSysSpecData->hSGXPCI, POULSBO_ADDR_RANGE_INDEX), OSPCIAddrRangeEnd(psSysSpecData->hSGXPCI, POULSBO_ADDR_RANGE_INDEX)));
#if defined(SGX_FEATURE_HOST_PORT)
	PVR_TRACE(("Host Port region: %x to %x", OSPCIAddrRangeStart(psSysSpecData->hSGXPCI, POULSBO_HP_ADDR_RANGE_INDEX), OSPCIAddrRangeEnd(psSysSpecData->hSGXPCI, POULSBO_HP_ADDR_RANGE_INDEX)));
#endif

	if (OSPCIAddrRangeLen(psSysSpecData->hSGXPCI, POULSBO_ADDR_RANGE_INDEX) < ui32MaxOffset)
	{
		PVR_DPF((PVR_DBG_ERROR,"PCIInitDev: Device memory region isn't big enough"));
		return PVRSRV_ERROR_PCI_REGION_TOO_SMALL;
	}


	if (OSPCIRequestAddrRange(psSysSpecData->hSGXPCI, POULSBO_ADDR_RANGE_INDEX) != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PCIInitDev: Device memory region not available"));
		return PVRSRV_ERROR_PCI_REGION_UNAVAILABLE;

	}
	 SYS_SPECIFIC_DATA_SET(psSysSpecData, SYS_SPECIFIC_DATA_PCI_REQUEST_SGX_ADDR_RANGE);

#if defined(SGX_FEATURE_HOST_PORT)

	if (OSPCIRequestAddrRange(psSysSpecData->hSGXPCI, POULSBO_HP_ADDR_RANGE_INDEX) != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"PCIInitDev: Host Port region not available"));
		return PVRSRV_ERROR_PCI_REGION_UNAVAILABLE;
	}
	 SYS_SPECIFIC_DATA_SET(psSysSpecData, SYS_SPECIFIC_DATA_PCI_REQUEST_HOST_PORT_RANGE);
#endif
	return PVRSRV_OK;
}

static IMG_VOID PCIDeInitDev(SYS_DATA *psSysData)
{
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_PCI_REQUEST_SGX_ADDR_RANGE))
	{
		OSPCIReleaseAddrRange(psSysSpecData->hSGXPCI, POULSBO_ADDR_RANGE_INDEX);
	}
#if defined(SGX_FEATURE_HOST_PORT)
	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_PCI_REQUEST_HOST_PORT_RANGE))
	{
		OSPCIReleaseAddrRange(psSysSpecData->hSGXPCI, POULSBO_HP_ADDR_RANGE_INDEX);
	}
#endif
	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_PCI_ACQUIRE_DEV))
	{
		OSPCIReleaseDev(psSysSpecData->hSGXPCI);
	}
}
static PVRSRV_ERROR SysLocateDevices(SYS_DATA *psSysData)
{
	IMG_UINT32 ui32BaseAddr = 0;
	IMG_UINT32 ui32IRQ = 0;
#if defined(SGX_FEATURE_HOST_PORT)
	IMG_UINT32 ui32HostPortAddr = 0;
#endif
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;

#if defined(SUPPORT_EXTERNAL_SYSTEM_CACHE)
    struct drm_psb_private *dev_priv = (struct drm_psb_private *) gpDrmDevice->dev_private;
#endif

	ui32BaseAddr = OSPCIAddrRangeStart(psSysSpecData->hSGXPCI, POULSBO_ADDR_RANGE_INDEX);
#if defined(SGX_FEATURE_HOST_PORT)
	ui32HostPortAddr = OSPCIAddrRangeStart(psSysSpecData->hSGXPCI, POULSBO_HP_ADDR_RANGE_INDEX);
#endif
	if (OSPCIIRQ(psSysSpecData->hSGXPCI, &ui32IRQ) != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysLocateDevices: Couldn't get IRQ"));
		return PVRSRV_ERROR_INVALID_DEVICE;
	}

	PVR_TRACE(("ui32BaseAddr: %08X", ui32BaseAddr));
#if defined(SGX_FEATURE_HOST_PORT)
	PVR_TRACE(("ui32HostPortAddr: %08X", ui32HostPortAddr));
#endif
	PVR_TRACE(("IRQ: %d", ui32IRQ));


	gsSGXDeviceMap.ui32Flags = 0x0;
	gsSGXDeviceMap.ui32IRQ = ui32IRQ;


#if defined(SUPPORT_DRI_DRM_EXT)
	gsSGXDeviceMap.sRegsSysPBase.uiAddr = ui32BaseAddr + ((IS_MRST(psSysSpecData->psDRMDev) || IS_MDFLD(psSysSpecData->psDRMDev)) ? MRST_SGX_REGS_OFFSET : POULSBO_SGX_REGS_OFFSET);
#else
	gsSGXDeviceMap.sRegsSysPBase.uiAddr = ui32BaseAddr + SGX_REGS_OFFSET;
#endif
	gsSGXDeviceMap.sRegsCpuPBase = SysSysPAddrToCpuPAddr(gsSGXDeviceMap.sRegsSysPBase);
	gsSGXDeviceMap.ui32RegsSize = SGX_REG_SIZE;

#if defined(SGX_FEATURE_HOST_PORT)

	gsSGXDeviceMap.ui32Flags = SGX_HOSTPORT_PRESENT;
	gsSGXDeviceMap.sHPSysPBase.uiAddr = ui32HostPortAddr;
	gsSGXDeviceMap.sHPCpuPBase = SysSysPAddrToCpuPAddr(gsSGXDeviceMap.sHPSysPBase);
	gsSGXDeviceMap.ui32HPSize = SYS_SGX_HP_SIZE;
#endif

#if defined(MRST_SLAVEPORT)

	gsSGXDeviceMap.sSPSysPBase.uiAddr = ui32BaseAddr + MRST_SGX_SP_OFFSET;
	gsSGXDeviceMap.sSPCpuPBase = SysSysPAddrToCpuPAddr(gsSGXDeviceMap.sSPSysPBase);
	gsSGXDeviceMap.ui32SPSize = SGX_SP_SIZE;
#endif




	gsSGXDeviceMap.sLocalMemSysPBase.uiAddr = 0;
	gsSGXDeviceMap.sLocalMemDevPBase.uiAddr = 0;
	gsSGXDeviceMap.sLocalMemCpuPBase.uiAddr = 0;
	gsSGXDeviceMap.ui32LocalMemSize = 0;

#if defined(SUPPORT_EXTERNAL_SYSTEM_CACHE)
	gsSGXDeviceMap.sExtSysCacheRegsDevPBase.uiAddr = SYS_EXT_SYS_CACHE_GBL_INV_REG_OFFSET;
	gsSGXDeviceMap.ui32ExtSysCacheRegsSize = SGX_EXT_SYSTEM_CACHE_REGS_SIZE;

	MDFLD_GL3_WRITE(gsSGXDeviceMap.sExtSysCacheRegsDevPBase.uiAddr, MDFLD_GL3_USE_WRT_INVAL);
#endif


#if !defined(NO_HARDWARE)

	{
		IMG_SYS_PHYADDR sPoulsboRegsCpuPBase;
		sPoulsboRegsCpuPBase.uiAddr = ui32BaseAddr + POULSBO_REGS_OFFSET;
		gsPoulsboRegsCPUVaddr = OSMapPhysToLin(SysSysPAddrToCpuPAddr(sPoulsboRegsCpuPBase),
												 POULSBO_REG_SIZE,
												 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
												 IMG_NULL);

		sPoulsboRegsCpuPBase.uiAddr = ui32BaseAddr + POULSBO_DISPLAY_REGS_OFFSET;
		gsPoulsboDisplayRegsCPUVaddr = OSMapPhysToLin(SysSysPAddrToCpuPAddr(sPoulsboRegsCpuPBase),
												 POULSBO_DISPLAY_REG_SIZE,
												 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
												 IMG_NULL);
	}
#endif
#if defined(PDUMP)
	{

		static IMG_CHAR pszPDumpDevName[] = "SGXMEM";
		gsSGXDeviceMap.pszPDumpDevName = pszPDumpDevName;
	}
#endif

	return PVRSRV_OK;
}


#define VERSION_STR_MAX_LEN_TEMPLATE "SGX revision = 000.000.000"
static PVRSRV_ERROR SysCreateVersionString(SYS_DATA *psSysData)
{
    IMG_UINT32 ui32MaxStrLen;
    PVRSRV_ERROR eError;
    IMG_INT32 i32Count;
    IMG_CHAR *pszVersionString;
    IMG_UINT32 ui32SGXRevision = 0;
	IMG_VOID *pvSGXRegs;

	pvSGXRegs = OSMapPhysToLin(gsSGXDeviceMap.sRegsCpuPBase,
											 gsSGXDeviceMap.ui32RegsSize,
											 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
											 IMG_NULL);

	if (pvSGXRegs != IMG_NULL)
	{
#if defined(SANTOS10_SGX_EARLY_WA)
            SysSGXEarlyWA(pvSGXRegs);
#endif
            ui32SGXRevision = OSReadHWReg(pvSGXRegs, EUR_CR_CORE_REVISION);
	     OSUnMapPhysToLin(pvSGXRegs,
		   									 	gsSGXDeviceMap.ui32RegsSize,
											 	PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
												IMG_NULL);
	}
	else
	{
	     PVR_DPF((PVR_DBG_ERROR,"SysCreateVersionString: Couldn't map SGX registers"));
	}

    ui32MaxStrLen = OSStringLength(VERSION_STR_MAX_LEN_TEMPLATE);
    eError = OSAllocMem(PVRSRV_OS_PAGEABLE_HEAP,
                          ui32MaxStrLen + 1,
                          (IMG_PVOID *)&pszVersionString,
                          IMG_NULL,
			  "Version String");
    if(eError != PVRSRV_OK)
    {
		return eError;
    }

    i32Count = OSSNPrintf(pszVersionString, ui32MaxStrLen + 1,
                           "SGX revision = %u.%u.%u",
                           (IMG_UINT)((ui32SGXRevision & EUR_CR_CORE_REVISION_MAJOR_MASK)
                            >> EUR_CR_CORE_REVISION_MAJOR_SHIFT),
                           (IMG_UINT)((ui32SGXRevision & EUR_CR_CORE_REVISION_MINOR_MASK)
                            >> EUR_CR_CORE_REVISION_MINOR_SHIFT),
                           (IMG_UINT)((ui32SGXRevision & EUR_CR_CORE_REVISION_MAINTENANCE_MASK)
                            >> EUR_CR_CORE_REVISION_MAINTENANCE_SHIFT)
                           );
    if(i32Count == -1)
    {
        ui32MaxStrLen = OSStringLength(VERSION_STR_MAX_LEN_TEMPLATE);
        OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
                    ui32MaxStrLen + 1,
                    pszVersionString,
                    IMG_NULL);

		return PVRSRV_ERROR_INVALID_PARAMS;
    }

    psSysData->pszVersionString = pszVersionString;

    return PVRSRV_OK;
}

static IMG_VOID SysFreeVersionString(SYS_DATA *psSysData)
{
    if(psSysData->pszVersionString)
    {
        IMG_UINT32 ui32MaxStrLen;
        ui32MaxStrLen = OSStringLength(VERSION_STR_MAX_LEN_TEMPLATE);
        OSFreeMem(PVRSRV_OS_PAGEABLE_HEAP,
                    ui32MaxStrLen+1,
                    psSysData->pszVersionString,
                    IMG_NULL);
		psSysData->pszVersionString = IMG_NULL;
    }
}

PVRSRV_ERROR SysInitialise(IMG_VOID)
{
	IMG_UINT32			i			  = 0;
	PVRSRV_ERROR 		eError;
	PVRSRV_DEVICE_NODE	*psDeviceNode;
	SGX_TIMING_INFORMATION*	psTimingInfo;
#if defined(PVR_MDFLD_SYS_MSVDX_AND_TOPAZ)
	struct drm_psb_private *dev_priv =
	    (struct drm_psb_private *) gpDrmDevice->dev_private;
#endif

	gpsSysData = &gsSysData;
	OSMemSet(gpsSysData, 0, sizeof(SYS_DATA));

	gpsSysData->pvSysSpecificData = &gsSysSpecificData;
	gsSysSpecificData.ui32SysSpecificData = 0;
#if defined(LDM_PCI) || defined(SUPPORT_DRI_DRM)

	PVR_ASSERT(gpsPVRLDMDev != IMG_NULL);
	gsSysSpecificData.psPCIDev = gpsPVRLDMDev;
#endif
#if defined(SUPPORT_DRI_DRM)

	PVR_ASSERT(gpsPVRDRMDev != IMG_NULL);
	gsSysSpecificData.psDRMDev = gpsPVRDRMDev;
#endif

	eError = OSInitEnvData(&gpsSysData->pvEnvSpecificData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to setup env structure"));
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return eError;
	}


	psTimingInfo = &gsSGXDeviceMap.sTimingInfo;
	psTimingInfo->ui32CoreClockSpeed = SYS_SGX_CLOCK_SPEED;
	psTimingInfo->ui32HWRecoveryFreq = SYS_SGX_HWRECOVERY_TIMEOUT_FREQ;
#if defined(SUPPORT_ACTIVE_POWER_MANAGEMENT)
	psTimingInfo->bEnableActivePM = IMG_TRUE;
#else
	psTimingInfo->bEnableActivePM = IMG_FALSE;
#endif
	psTimingInfo->ui32ActivePowManLatencyms = SYS_SGX_ACTIVE_POWER_LATENCY_MS;
	psTimingInfo->ui32uKernelFreq = SYS_SGX_PDS_TIMER_FREQ;

	eError = PCIInitDev(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return eError;
	}

	gpsSysData->ui32NumDevices = SYS_DEVICE_COUNT;


	for(i=0; i<SYS_DEVICE_COUNT; i++)
	{
		gpsSysData->sDeviceID[i].uiID = i;
		gpsSysData->sDeviceID[i].bInUse = IMG_FALSE;
	}

	gpsSysData->psDeviceNodeList = IMG_NULL;
	gpsSysData->psQueueList = IMG_NULL;

	eError = SysInitialiseCommon(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed in SysInitialiseCommon"));
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return eError;
	}





	eError = SysLocateDevices(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to locate devices"));
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return eError;
	}


	eError = PVRSRVRegisterDevice(gpsSysData, SGXRegisterDevice,
								  DEVICE_SGX_INTERRUPT, &gui32SGXDeviceID);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to register device!"));
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return eError;
	}

	psDeviceNode = gpsSysData->psDeviceNodeList;

	while(psDeviceNode)
	{

		switch(psDeviceNode->sDevId.eDeviceType)
		{
			case PVRSRV_DEVICE_TYPE_SGX:
			{
				DEVICE_MEMORY_INFO *psDevMemoryInfo;
				DEVICE_MEMORY_HEAP_INFO *psDeviceMemoryHeap;


				psDeviceNode->psLocalDevMemArena = IMG_NULL;


				psDevMemoryInfo = &psDeviceNode->sDevMemoryInfo;
				psDeviceMemoryHeap = psDevMemoryInfo->psDeviceMemoryHeap;


				for(i=0; i<psDevMemoryInfo->ui32HeapCount; i++)
				{
					psDeviceMemoryHeap[i].ui32Attribs |= PVRSRV_BACKINGSTORE_SYSMEM_NONCONTIG;
#ifdef OEM_CUSTOMISE

#endif
				}
#if defined(SUPPORT_DRI_DRM_EXT)
				gpsSGXDevNode = psDeviceNode;
#endif
				break;
			}
#if defined(PVR_MDFLD_SYS_MSVDX_AND_TOPAZ)
			case PVRSRV_DEVICE_TYPE_MSVDX:

			break;
			case PVRSRV_DEVICE_TYPE_TOPAZ:
			break;
#endif
			default:
			{
				PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to find SGX device node!"));
				return PVRSRV_ERROR_INIT_FAILURE;
			}
		}


		psDeviceNode = psDeviceNode->psNext;
	}

	PDUMPINIT();
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_PDUMP_INIT);


	eError = PVRSRVInitialiseDevice (gui32SGXDeviceID);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to initialise device!"));
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return eError;
	}
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_SGX_INITIALISED);

#if defined(PVR_MDFLD_SYS_MSVDX_AND_TOPAZ)
	eError = PVRSRVInitialiseDevice (gui32MRSTMSVDXDeviceID);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to initialise device!"));
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return eError;
	}

	if (IS_MDFLD(gpDrmDevice) && !dev_priv->topaz_disabled)
	{
		eError = PVRSRVInitialiseDevice (gui32MRSTTOPAZDeviceID);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to initialise device!"));
			SysDeinitialise(gpsSysData);
			gpsSysData = IMG_NULL;
			return eError;
		}
	}
#endif

#if defined(SANTOS10_SGX_DC_NOHW)
	/*
	 * Cycle 10/11: publish the /proc control surface only after every
	 * Services prerequisite used by DC_NOHW_Init/Init is fully initialised:
	 *   - SYS_DATA zeroed and env data / PCI initialised        (above)
	 *   - sDeviceID[] table + ui32NumDevices                    (above)
	 *   - psDeviceNodeList / psQueueList                        (above)
	 *   - SysInitialiseCommon + SysLocateDevices                (above)
	 *   - SGX device node registered and initialised            (above)
	 * DC Init relies on the device-ID table (AllocateDeviceID),
	 * the device-node list (List_PVRSRV_DEVICE_NODE_Insert),
	 * SysRegisterExternalDevice and the command-processor table
	 * (apsDeviceCommandData). Publishing earlier (Cycle 9) let a userspace
	 * write reach DC_NOHW_Init while SysInitialise was still resetting that
	 * state, creating DC/cmdproc objects that the main thread then
	 * orphaned.
	 *
	 * Cycle 11: the entry is created UNARMED. It exists before the
	 * automatic enable below (so a successfully RUNNING pinned DC always
	 * has its cleanup control), but writes are rejected with -EAGAIN until
	 * DC_NOHW_ProcArm() is the final commit action. Without this, an
	 * external write could re-enable DC between SysDeinitialise's
	 * DC_NOHW_Cleanup() and DC_NOHW_ProcExit() during the automatic-enable
	 * failure unwind, leaving a RUNNING pinned DC while Services is torn
	 * down (the module pin does not protect the SysInitialise failure path
	 * because the module is not GOING there).
	 *
	 * proc_create failure here unwinds the already-initialised Services
	 * through the same SysDeinitialise path used by the DC-init failure
	 * branch below (no entry exists, control never armed, DC is OFF, so
	 * DC_NOHW_Cleanup is a no-op and DC_NOHW_ProcExit sees no entry).
	 */
	if (DC_NOHW_ProcInit() != 0)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: DC_NOHW_ProcInit failed"));
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return PVRSRV_ERROR_INIT_FAILURE;
	}

	if (g_santos10_dc_nohw_enable && (DC_NOHW_Init() != 0))
	{
		/*
		 * Cycle 11 pre-commit unwind: control was never armed, so no
		 * external write can have changed DC state, and the failed
		 * DC_NOHW_Init() is self-unwinding (early pin released, lifecycle
		 * OFF). Shut the unarmed proc surface down first (remove_proc_entry
		 * -> proc_entry_rundown waits for any in-flight callback) BEFORE
		 * the destructive Services unwind, so no callback can re-enable DC
		 * while SysDeinitialise destroys MISR/SGX/command-processor state.
		 * With the lifecycle provably OFF, SysDeinitialise's
		 * DC_NOHW_Cleanup() cannot return PROCESSING_BLOCKED here, so the
		 * discarded SysDeinitialise return value cannot hide retryable DC
		 * ownership.
		 */
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: DC_NOHW_Init failed"));
		DC_NOHW_ProcExit();
		SysDeinitialise(gpsSysData);
		gpsSysData = IMG_NULL;
		return PVRSRV_ERROR_INIT_FAILURE;
	}

	/*
	 * No fallible SysInitialise work remains in this configuration (the
	 * DRM build returns straight to PVRCore_Init after SysInitialise):
	 * arm the runtime control surface as the final commit action. This
	 * store cannot fail, so there is no rollback path past this point.
	 */
	DC_NOHW_ProcArm();
#endif
	return PVRSRV_OK;
}


#if !defined(SUPPORT_DRI_DRM_EXT)
static IMG_VOID SysEnableInterrupts(SYS_DATA *psSysData)
{
#if !defined(NO_HARDWARE)
	IMG_UINT32 ui32RegData;
	IMG_UINT32 ui32Mask;

	ui32Mask = POULSBO_THALIA_MASK;


	ui32RegData = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_IDENTITY_REG);
	OSWriteHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_IDENTITY_REG, ui32RegData | ui32Mask);


	ui32RegData = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_MASK_REG);
	OSWriteHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_MASK_REG, ui32RegData & (~ui32Mask));


	ui32RegData = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_ENABLE_REG);
	OSWriteHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_ENABLE_REG, ui32RegData | ui32Mask);

	PVR_DPF((PVR_DBG_MESSAGE, "SysEnableInterrupts: Interrupts enabled"));
#endif
	PVR_UNREFERENCED_PARAMETER(psSysData);
}
#endif

#if !defined(SUPPORT_DRI_DRM_EXT)
static IMG_VOID SysDisableInterrupts(SYS_DATA *psSysData)
{
#if !defined(NO_HARDWARE)
	IMG_UINT32 ui32RegData;
	IMG_UINT32 ui32Mask;
	ui32Mask = POULSBO_THALIA_MASK;


	ui32RegData = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_ENABLE_REG);
	OSWriteHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_ENABLE_REG, ui32RegData & (~ui32Mask));


	ui32RegData = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_MASK_REG);
	OSWriteHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_MASK_REG, ui32RegData | ui32Mask);

	PVR_TRACE(("SysDisableInterrupts: Interrupts disabled"));
#endif
	PVR_UNREFERENCED_PARAMETER(psSysData);
}
#endif

PVRSRV_ERROR SysFinalise(IMG_VOID)
{
	PVRSRV_ERROR eError = PVRSRV_OK;


	eError = OSInstallMISR(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysFinalise: OSInstallMISR failed"));
		return eError;
	}
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_MISR_INSTALLED);

#if defined(SYS_USING_INTERRUPTS) && defined(SUPPORT_DRI_DRM_EXT)
	/* The standalone DRM shell replaces golden psb_irq's callback seam. */
	if (santos_pvr_irq_start(gsPoulsboRegsCPUVaddr,
				 gsPoulsboDisplayRegsCPUVaddr,
				 SYSPVRServiceSGXInterrupt)) {
		PVR_DPF((PVR_DBG_ERROR, "SysFinalise: Santos SGX MSI install failed"));
		return PVRSRV_ERROR_INIT_FAILURE;
	}
	if (OSPCIIRQ(gsSysSpecificData.hSGXPCI, &gsSGXDeviceMap.ui32IRQ)
		!= PVRSRV_OK || !gsSGXDeviceMap.ui32IRQ) {
		santos_pvr_irq_stop();
		return PVRSRV_ERROR_INIT_FAILURE;
	}
#endif

#if defined(SYS_USING_INTERRUPTS) && !defined(SUPPORT_DRI_DRM_EXT)
	eError = OSInstallSystemLISR(gpsSysData, gsSGXDeviceMap.ui32IRQ);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysFinalise: OSInstallSystemLISR failed"));
		return eError;
	}
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_LISR_INSTALLED);
#endif

#if (defined(SYS_USING_INTERRUPTS) && !defined(SUPPORT_DRI_DRM_EXT))
	SysEnableInterrupts(gpsSysData);
	SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_IRQ_ENABLED);
#endif

#if defined(__linux__)

	eError = SysCreateVersionString(gpsSysData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysInitialise: Failed to create a system version string"));
	}
	else
	{
	    PVR_DPF((PVR_DBG_WARNING, "SysFinalise: Version string: %s", gpsSysData->pszVersionString));
	}
#endif

	return eError;
}

PVRSRV_ERROR SysDeinitialise (SYS_DATA *psSysData)
{
	PVRSRV_ERROR eError;
	SYS_SPECIFIC_DATA *psSysSpecData = (SYS_SPECIFIC_DATA *) psSysData->pvSysSpecificData;

#if defined(SYS_USING_INTERRUPTS) && defined(SUPPORT_DRI_DRM_EXT)
	/* No ISR may observe torn-down DC, MISR, device data or BAR mappings. */
	santos_pvr_irq_stop();
#endif

#if defined(SANTOS10_SGX_DC_NOHW)
	/*
	 * Cycle 8: status-bearing DC cleanup contract. DC_NOHW_Cleanup()
	 * returns DC_OK only when the display controller reached the OFF
	 * topology. A failure means DC/cmdproc/queue ownership may still be
	 * live, so Services teardown must NOT proceed past this point: abort
	 * and leave the rest of the system data intact for a retry.
	 *
	 * The normal module-unload path cannot even reach here while DC is
	 * active: DC_NOHW_Init() holds one THIS_MODULE pin for the RUNNING/
	 * STAGED lifetime, so rmmod fails with EBUSY until a successful
	 * disable. This check is the defense-in-depth backstop for the
	 * remaining case (an explicit reload/exit path with DC already OFF).
	 */
	if (DC_NOHW_Cleanup() != 0)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysDeinitialise: DC_NOHW cleanup failed, aborting teardown"));
		return PVRSRV_ERROR_PROCESSING_BLOCKED;
	}
	DC_NOHW_ProcExit();
#endif

	PVR_UNREFERENCED_PARAMETER(psSysData);

#if (defined(SYS_USING_INTERRUPTS) && !defined(SUPPORT_DRI_DRM_EXT))
	if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_IRQ_ENABLED))
	{
		SysDisableInterrupts(psSysData);
	}
#endif

#if defined(SYS_USING_INTERRUPTS) && !defined(SUPPORT_DRI_DRM_EXT)
	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_LISR_INSTALLED))
	{
		eError = OSUninstallSystemLISR(gpsSysData);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR,"SysDeinitialise: OSUninstallSystemLISR failed"));
			return eError;
		}
	}
#endif

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_MISR_INSTALLED))
	{
		eError = OSUninstallMISR(gpsSysData);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR,"SysDeinitialise: OSUninstallMISR failed"));
			return eError;
		}
	}

	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_SGX_INITIALISED))
	{

		eError = PVRSRVDeinitialiseDevice(gui32SGXDeviceID);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR,"SysDeinitialise: failed to de-init the device"));
			return eError;
		}
	}

	SysFreeVersionString(psSysData);

	PCIDeInitDev(psSysData);

	eError = OSDeInitEnvData(psSysData->pvEnvSpecificData);
	if (eError != PVRSRV_OK)
	{
		PVR_DPF((PVR_DBG_ERROR,"SysDeinitialise: failed to de-init env structure"));
		return eError;
	}

	SysDeinitialiseCommon(gpsSysData);


#if !defined(NO_HARDWARE)

	OSUnMapPhysToLin(gsPoulsboRegsCPUVaddr,
											 POULSBO_REG_SIZE,
											 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
											 IMG_NULL);

	OSUnMapPhysToLin(gsPoulsboDisplayRegsCPUVaddr,
											 POULSBO_DISPLAY_REG_SIZE,
											 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
											 IMG_NULL);
#endif
	if (SYS_SPECIFIC_DATA_TEST(psSysSpecData, SYS_SPECIFIC_DATA_PDUMP_INIT))
	{
		PDUMPDEINIT();
	}

	gpsSysData = IMG_NULL;

	return PVRSRV_OK;
}


IMG_UINT32 SysGetInterruptSource(SYS_DATA* psSysData,
								 PVRSRV_DEVICE_NODE *psDeviceNode)
{
#if !defined(SUPPORT_DRI_DRM_EXT)
	IMG_UINT32 ui32Devices = 0;
	IMG_UINT32 ui32Data, ui32DIMMask;

	PVR_UNREFERENCED_PARAMETER(psSysData);
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);


	ui32Data = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_IDENTITY_REG);

	if (ui32Data & POULSBO_THALIA_MASK)
	{
		ui32Devices |= DEVICE_SGX_INTERRUPT;
	}

	if (ui32Data & POULSBO_MSVDX_MASK)
	{
		ui32Devices |= DEVICE_MSVDX_INTERRUPT;
	}


	ui32DIMMask = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_ENABLE_REG);
	ui32DIMMask &= ~(POULSBO_THALIA_MASK | POULSBO_MSVDX_MASK);


	if (ui32Data & ui32DIMMask)
	{
		ui32Devices |= DEVICE_DISP_INTERRUPT;
	}

	return (ui32Devices);
#else
	PVR_UNREFERENCED_PARAMETER(psSysData);
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);

	return 0;
#endif
}

IMG_VOID SysClearInterrupts(SYS_DATA* psSysData, IMG_UINT32 ui32ClearBits)
{
#if !defined(SUPPORT_DRI_DRM_EXT)
	IMG_UINT32 ui32Data;
	IMG_UINT32 ui32Mask = 0;

	PVR_UNREFERENCED_PARAMETER(psSysData);

	ui32Data = OSReadHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_IDENTITY_REG);

	if ((ui32ClearBits & DEVICE_SGX_INTERRUPT) &&
		(ui32Data & POULSBO_THALIA_MASK))
	{
		ui32Mask |= POULSBO_THALIA_MASK;
	}

	if ((ui32ClearBits & DEVICE_MSVDX_INTERRUPT) &&
		(ui32Data & POULSBO_MSVDX_MASK))
	{
		ui32Mask |= POULSBO_MSVDX_MASK;
	}

	if ((ui32ClearBits & DEVICE_DISP_INTERRUPT) &&
		(ui32Data & POULSBO_VSYNC_PIPEA_VBLANK_MASK))
	{
	  ui32Mask |= POULSBO_VSYNC_PIPEA_VBLANK_MASK;
	}

	if (ui32Mask)
	{
		OSWriteHWReg(gsPoulsboRegsCPUVaddr, POULSBO_INTERRUPT_IDENTITY_REG, ui32Mask);
	}
#else
	PVR_UNREFERENCED_PARAMETER(psSysData);
	PVR_UNREFERENCED_PARAMETER(ui32ClearBits);
#endif
}


PVRSRV_ERROR SysGetDeviceMemoryMap(PVRSRV_DEVICE_TYPE eDeviceType,
									IMG_VOID **ppvDeviceMap)
{
	switch(eDeviceType)
	{
		case PVRSRV_DEVICE_TYPE_SGX:
		{

			*ppvDeviceMap = (IMG_VOID*)&gsSGXDeviceMap;
			break;
		}
		default:
		{
			PVR_DPF((PVR_DBG_ERROR,"SysGetDeviceMemoryMap: unsupported device type"));
		}
	}
	return PVRSRV_OK;
}


IMG_DEV_PHYADDR SysCpuPAddrToDevPAddr (PVRSRV_DEVICE_TYPE eDeviceType,
										IMG_CPU_PHYADDR CpuPAddr)
{
	IMG_DEV_PHYADDR DevPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);


	DevPAddr.uiAddr = CpuPAddr.uiAddr;

	return DevPAddr;
}


IMG_CPU_PHYADDR SysSysPAddrToCpuPAddr (IMG_SYS_PHYADDR sys_paddr)
{
	IMG_CPU_PHYADDR cpu_paddr;


	cpu_paddr.uiAddr = sys_paddr.uiAddr;
	return cpu_paddr;
}

IMG_SYS_PHYADDR SysCpuPAddrToSysPAddr (IMG_CPU_PHYADDR cpu_paddr)
{
	IMG_SYS_PHYADDR sys_paddr;


	sys_paddr.uiAddr = cpu_paddr.uiAddr;
	return sys_paddr;
}


IMG_DEV_PHYADDR SysSysPAddrToDevPAddr (PVRSRV_DEVICE_TYPE eDeviceType, IMG_SYS_PHYADDR SysPAddr)
{
    IMG_DEV_PHYADDR DevPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);


    DevPAddr.uiAddr = SysPAddr.uiAddr;

    return DevPAddr;
}


IMG_SYS_PHYADDR SysDevPAddrToSysPAddr (PVRSRV_DEVICE_TYPE eDeviceType, IMG_DEV_PHYADDR DevPAddr)
{
    IMG_SYS_PHYADDR SysPAddr;

	PVR_UNREFERENCED_PARAMETER(eDeviceType);


    SysPAddr.uiAddr = DevPAddr.uiAddr;

    return SysPAddr;
}


IMG_VOID SysRegisterExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{

  psDeviceNode->ui32SOCInterruptBit = DEVICE_DISP_INTERRUPT;
}


IMG_VOID SysRemoveExternalDevice(PVRSRV_DEVICE_NODE *psDeviceNode)
{
	PVR_UNREFERENCED_PARAMETER(psDeviceNode);
}

PVRSRV_ERROR SysOEMFunction (	IMG_UINT32	ui32ID,
								IMG_VOID	*pvIn,
								IMG_UINT32  ulInSize,
								IMG_VOID	*pvOut,
								IMG_UINT32	ulOutSize)
{
	PVR_UNREFERENCED_PARAMETER(ulInSize);
	PVR_UNREFERENCED_PARAMETER(pvIn);

#if !defined(SUPPORT_DRI_DRM_EXT)
	if ((ui32ID == OEM_GET_EXT_FUNCS) &&
		(ulOutSize == sizeof(PVRSRV_DC_OEM_JTABLE)))
	{
		PVRSRV_DC_OEM_JTABLE *psOEMJTable = (PVRSRV_DC_OEM_JTABLE*)pvOut;
		psOEMJTable->pfnOEMBridgeDispatch = &PVRSRV_BridgeDispatchKM;

		psOEMJTable->pfnOEMReadRegistryString  = IMG_NULL;
		psOEMJTable->pfnOEMWriteRegistryString = IMG_NULL;

		return PVRSRV_OK;
	}
#endif

	return PVRSRV_ERROR_INVALID_PARAMS;
}


#if !defined(SUPPORT_DRI_DRM_EXT)
static PVRSRV_ERROR SysMapInRegisters(IMG_VOID)
{
	PVRSRV_DEVICE_NODE *psDeviceNodeList;

	psDeviceNodeList = gpsSysData->psDeviceNodeList;

	while (psDeviceNodeList)
	{
		switch(psDeviceNodeList->sDevId.eDeviceType)
		{
		case PVRSRV_DEVICE_TYPE_SGX:
		{
			PVRSRV_SGXDEV_INFO *psDevInfo = (PVRSRV_SGXDEV_INFO *)psDeviceNodeList->pvDevice;

			if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNMAP_SGX_REGS))
			{
				psDevInfo->pvRegsBaseKM = OSMapPhysToLin(gsSGXDeviceMap.sRegsCpuPBase,
									 gsSGXDeviceMap.ui32RegsSize,
									 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
									 IMG_NULL);

				if (!psDevInfo->pvRegsBaseKM)
				{
					PVR_DPF((PVR_DBG_ERROR,"SysMapInRegisters : Failed to map in SGX registers\n"));
					return PVRSRV_ERROR_BAD_MAPPING;
				}
				SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNMAP_SGX_REGS);
			}
			psDevInfo->ui32RegSize   = gsSGXDeviceMap.ui32RegsSize;
			psDevInfo->sRegsPhysBase = gsSGXDeviceMap.sRegsSysPBase;

#if defined(SGX_FEATURE_HOST_PORT)
			if (gsSGXDeviceMap.ui32Flags & SGX_HOSTPORT_PRESENT)
			{
				if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNMAP_SGX_HP))
				{

					psDevInfo->pvHostPortBaseKM = OSMapPhysToLin(gsSGXDeviceMap.sHPCpuPBase,
														     gsSGXDeviceMap.ui32HPSize,
														     PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
														     IMG_NULL);
					if (!psDevInfo->pvHostPortBaseKM)
					{
						PVR_DPF((PVR_DBG_ERROR,"SysMapInRegisters : Failed to map in host port\n"));
						return PVRSRV_ERROR_BAD_MAPPING;
					}
					SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNMAP_SGX_HP);
				}
				psDevInfo->ui32HPSize  = gsSGXDeviceMap.ui32HPSize;
				psDevInfo->sHPSysPAddr = gsSGXDeviceMap.sHPSysPBase;
			}
#endif
			break;
		}
		default:
			break;
		}
		psDeviceNodeList = psDeviceNodeList->psNext;
	}

	return PVRSRV_OK;
}


static PVRSRV_ERROR SysUnmapRegisters(IMG_VOID)
{
	PVRSRV_DEVICE_NODE *psDeviceNodeList;

	psDeviceNodeList = gpsSysData->psDeviceNodeList;

	while (psDeviceNodeList)
	{
		switch (psDeviceNodeList->sDevId.eDeviceType)
		{
		case PVRSRV_DEVICE_TYPE_SGX:
		{
			PVRSRV_SGXDEV_INFO *psDevInfo = (PVRSRV_SGXDEV_INFO *)psDeviceNodeList->pvDevice;
#if !(defined(NO_HARDWARE) && defined(__linux__))

			if (psDevInfo->pvRegsBaseKM)
			{
				OSUnMapPhysToLin(psDevInfo->pvRegsBaseKM,
				                 gsSGXDeviceMap.ui32RegsSize,
				                 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
				                 IMG_NULL);

				SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNMAP_SGX_REGS);
			}
#endif

			psDevInfo->pvRegsBaseKM = IMG_NULL;
			psDevInfo->ui32RegSize          = 0;
			psDevInfo->sRegsPhysBase.uiAddr = 0;

#if defined(SGX_FEATURE_HOST_PORT)
			if (gsSGXDeviceMap.ui32Flags & SGX_HOSTPORT_PRESENT)
			{

				if (psDevInfo->pvHostPortBaseKM)
				{
					OSUnMapPhysToLin(psDevInfo->pvHostPortBaseKM,
					                 gsSGXDeviceMap.ui32HPSize,
					                 PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
					                 IMG_NULL);

					SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNMAP_SGX_HP);

					psDevInfo->pvHostPortBaseKM = IMG_NULL;
				}

				psDevInfo->ui32HPSize  = 0;
				psDevInfo->sHPSysPAddr.uiAddr = 0;
			}
#endif
			break;
		}
		default:
			break;
		}
		psDeviceNodeList = psDeviceNodeList->psNext;
	}

#if !(defined(NO_HARDWARE) || defined(__linux__))

	OSUnMapPhysToLin(gsPoulsboRegsCPUVaddr,
				POULSBO_REG_SIZE,
				PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
				IMG_NULL);


	OSUnMapPhysToLin(gsPoulsboDisplayRegsCPUVaddr,
				POULSBO_DISPLAY_REG_SIZE,
				PVRSRV_HAP_KERNEL_ONLY|PVRSRV_HAP_UNCACHED,
				IMG_NULL);

#endif

	return PVRSRV_OK;
}
#endif


PVRSRV_ERROR SysSystemPrePowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
#if !defined(SUPPORT_DRI_DRM_EXT)
	PVRSRV_ERROR eError= PVRSRV_OK;

	if (eNewPowerState != gpsSysData->eCurrentPowerState)
	{
		if ((eNewPowerState == PVRSRV_SYS_POWER_STATE_D3) &&
			(gpsSysData->eCurrentPowerState < PVRSRV_SYS_POWER_STATE_D3))
		{

			if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_IRQ_ENABLED))
			{
				SysDisableInterrupts(gpsSysData);

				SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_IRQ_DISABLE);
				SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_IRQ_ENABLED);
			}

#if defined(SYS_USING_INTERRUPTS)
			if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_LISR_INSTALLED))
			{
				eError = OSUninstallSystemLISR(gpsSysData);
				if (eError != PVRSRV_OK)
				{
					PVR_DPF((PVR_DBG_ERROR,"SysSystemPrePowerState: OSUninstallSystemLISR failed (%d)", eError));
				}
				SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNINSTALL_LISR);
				SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_LISR_INSTALLED);
			}
#endif


			SysUnmapRegisters();

			eError = OSPCISuspendDev(gsSysSpecificData.hSGXPCI);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR,"SysSystemPrePowerState: OSPCISuspendDev failed (%d)", eError));
			}
		}
	}
	return eError;
#else
	PVR_UNREFERENCED_PARAMETER(eNewPowerState);

	return PVRSRV_ERROR_NOT_SUPPORTED;
#endif
}

PVRSRV_ERROR SysSystemPostPowerState(PVRSRV_SYS_POWER_STATE eNewPowerState)
{
#if !defined(SUPPORT_DRI_DRM_EXT)
	PVRSRV_ERROR eError = PVRSRV_OK;

	if (eNewPowerState != gpsSysData->eCurrentPowerState)
	{
		if ((gpsSysData->eCurrentPowerState == PVRSRV_SYS_POWER_STATE_D3) &&
			(eNewPowerState < PVRSRV_SYS_POWER_STATE_D3))
		{
			eError = OSPCIResumeDev(gsSysSpecificData.hSGXPCI);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR,"SysSystemPostPowerState: OSPCIResumeDev failed (%d)", eError));
				return eError;
			}




			eError = SysLocateDevices(gpsSysData);
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR,"SysSystemPostPowerState: Failed to locate devices"));
				return eError;
			}


			eError = SysMapInRegisters();
			if (eError != PVRSRV_OK)
			{
				PVR_DPF((PVR_DBG_ERROR,"SysSystemPostPowerState: Failed to map in registers"));
				return eError;
			}

#if defined(SYS_USING_INTERRUPTS)
			if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNINSTALL_LISR))
			{
				eError = OSInstallSystemLISR(gpsSysData, gsSGXDeviceMap.ui32IRQ);
				if (eError != PVRSRV_OK)
				{
					PVR_DPF((PVR_DBG_ERROR,"SysSystemPostPowerState: OSInstallSystemLISR failed to install ISR (%d)", eError));
				}
				SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_LISR_INSTALLED);
				SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_UNINSTALL_LISR);
			}
#endif

			if (SYS_SPECIFIC_DATA_TEST(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_IRQ_DISABLE))
			{
				SysEnableInterrupts(gpsSysData);

				SYS_SPECIFIC_DATA_SET(&gsSysSpecificData, SYS_SPECIFIC_DATA_IRQ_ENABLED);
				SYS_SPECIFIC_DATA_CLEAR(&gsSysSpecificData, SYS_SPECIFIC_DATA_PM_IRQ_DISABLE);
			}
		}
	}
	return eError;
#else
	PVR_UNREFERENCED_PARAMETER(eNewPowerState);

	return PVRSRV_ERROR_NOT_SUPPORTED;
#endif
}


PVRSRV_ERROR SysDevicePrePowerState(IMG_UINT32			ui32DeviceIndex,
									PVRSRV_DEV_POWER_STATE	eNewPowerState,
									PVRSRV_DEV_POWER_STATE	eCurrentPowerState)
{
	if ((eNewPowerState != eCurrentPowerState) &&
	    (eNewPowerState == PVRSRV_DEV_POWER_STATE_OFF))
	{
		if (ui32DeviceIndex == gui32SGXDeviceID)
		{
			PVR_DPF((PVR_DBG_MESSAGE,"SysDevicePrePowerState: Remove SGX power"));
#if defined(SUPPORT_DRI_DRM_EXT)
			ospm_power_using_hw_end(OSPM_GRAPHICS_ISLAND);

			ospm_power_graphics_island_down(OSPM_GRAPHICS_ISLAND);
#endif
#ifdef CONFIG_MDFD_GL3
			/* Power off GL3 */
			ospm_power_island_down(OSPM_GL3_CACHE_ISLAND);
#endif

#if defined(SUPPORT_DRI_DRM_EXT)
			ospm_power_using_hw_end(OSPM_DISPLAY_ISLAND);
#endif

		}
#if defined(PVR_MDFLD_SYS_MSVDX_AND_TOPAZ)
		else if (ui32DeviceIndex == gui32MRSTMSVDXDeviceID)
		{
			psb_irq_uninstall_islands(gpDrmDevice, OSPM_VIDEO_DEC_ISLAND);
			if (ospm_power_is_hw_on(OSPM_DISPLAY_ISLAND)) {
				ospm_power_island_down(OSPM_VIDEO_DEC_ISLAND);
			} else {
				ospm_power_island_up(OSPM_DISPLAY_ISLAND);
				ospm_power_island_down(OSPM_VIDEO_DEC_ISLAND);
				ospm_power_island_down(OSPM_DISPLAY_ISLAND);
			}
#if 0
#if defined(SUPPORT_DRI_DRM_EXT)
			ospm_power_using_hw_end(OSPM_VIDEO_DEC_ISLAND);
			ospm_power_using_hw_end(OSPM_DISPLAY_ISLAND);
#endif
#endif
		}
		else if (ui32DeviceIndex == gui32MRSTTOPAZDeviceID)
		{
		if (ospm_power_is_hw_on(OSPM_DISPLAY_ISLAND)) {
				ospm_power_island_down(OSPM_VIDEO_ENC_ISLAND);
			} else {
				ospm_power_island_up(OSPM_DISPLAY_ISLAND);
				ospm_power_island_down(OSPM_VIDEO_ENC_ISLAND);
				ospm_power_island_down(OSPM_DISPLAY_ISLAND);
			}
		}
#if 0
#if defined(SUPPORT_DRI_DRM_EXT)
			ospm_power_using_hw_end(OSPM_VIDEO_ENC_ISLAND);
			ospm_power_using_hw_end(OSPM_DISPLAY_ISLAND);
#endif
#endif
#endif
	}
	return PVRSRV_OK;
}


PVRSRV_ERROR SysDevicePostPowerState(IMG_UINT32			ui32DeviceIndex,
									 PVRSRV_DEV_POWER_STATE	eNewPowerState,
									 PVRSRV_DEV_POWER_STATE	eCurrentPowerState)
{
	if ((eNewPowerState != eCurrentPowerState) &&
	    (eCurrentPowerState == PVRSRV_DEV_POWER_STATE_OFF))
	{
		if (ui32DeviceIndex == gui32SGXDeviceID)
		{
			PVR_DPF((PVR_DBG_MESSAGE,"SysDevicePrePowerState: Restore SGX power"));
#if defined(SUPPORT_DRI_DRM_EXT)
			if (!ospm_power_using_hw_begin(OSPM_DISPLAY_ISLAND, OSPM_UHB_FORCE_POWER_ON))
			{
				return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
			}

			if (!ospm_power_using_hw_begin(OSPM_GRAPHICS_ISLAND, OSPM_UHB_FORCE_POWER_ON))
			{
				ospm_power_using_hw_end(OSPM_DISPLAY_ISLAND);

				return PVRSRV_ERROR_DEVICE_POWER_CHANGE_FAILURE;
			}
#endif
		}
#if defined(PVR_MDFLD_SYS_MSVDX_AND_TOPAZ)
		else if (ui32DeviceIndex == gui32MRSTMSVDXDeviceID)
		{
			PVR_DPF((PVR_DBG_MESSAGE,"SysDevicePrePowerState: Restore MSVDX power"));
			if (ospm_power_is_hw_on(OSPM_DISPLAY_ISLAND)) {
				ospm_power_island_up(OSPM_VIDEO_DEC_ISLAND);
			} else {
				ospm_power_island_up(OSPM_DISPLAY_ISLAND);
				ospm_power_island_up(OSPM_VIDEO_DEC_ISLAND);
				ospm_power_island_down(OSPM_DISPLAY_ISLAND);
			}
#if 0
#if defined(SUPPORT_DRI_DRM_EXT)
			if (!ospm_power_using_hw_begin(OSPM_DISPLAY_ISLAND, OSPM_UHB_FORCE_POWER_ON))
			{
				return PVRSRV_ERROR_GENERIC;
			}

			if (!ospm_power_using_hw_begin(OSPM_VIDEO_DEC_ISLAND, true))
			{
				ospm_power_using_hw_end(OSPM_DISPLAY_ISLAND);

				return PVRSRV_ERROR_GENERIC;
			}
#endif
#endif
		}
		else if (ui32DeviceIndex == gui32MRSTTOPAZDeviceID)
		{
			PVR_DPF((PVR_DBG_MESSAGE,"SysDevicePrePowerState: Restore TOPAZ power"));
			if (ospm_power_is_hw_on(OSPM_DISPLAY_ISLAND)) {
				ospm_power_island_up(OSPM_VIDEO_ENC_ISLAND);
			} else {
				ospm_power_island_up(OSPM_DISPLAY_ISLAND);
				ospm_power_island_up(OSPM_VIDEO_ENC_ISLAND);
				ospm_power_island_down(OSPM_DISPLAY_ISLAND);
			}
#if 0
#if defined(SUPPORT_DRI_DRM_EXT)
			if (!ospm_power_using_hw_begin(OSPM_DISPLAY_ISLAND, OSPM_UHB_FORCE_POWER_ON))
			{
				return PVRSRV_ERROR_GENERIC;
			}

			if (!ospm_power_using_hw_begin(OSPM_VIDEO_ENC_ISLAND, true))
			{
				ospm_power_using_hw_end(OSPM_DISPLAY_ISLAND);

				return PVRSRV_ERROR_GENERIC;
			}
#endif
#endif
		}
#endif
	}

	return PVRSRV_OK;
}

#if defined(SYS_SUPPORTS_SGX_IDLE_CALLBACK)

IMG_VOID SysSGXIdleTransition(IMG_BOOL bSGXIdle)
{
	PVR_DPF((PVR_DBG_MESSAGE, "SysSGXIdleTransition switch to %u", bSGXIdle));
}

#endif

#if defined(SUPPORT_DRI_DRM_EXT)
int SYSPVRServiceSGXInterrupt(struct drm_device *dev)
{
	IMG_BOOL bStatus = IMG_FALSE;

	PVR_UNREFERENCED_PARAMETER(dev);

	if (gpsSGXDevNode != IMG_NULL)
	{
		bStatus = (*gpsSGXDevNode->pfnDeviceISR)(gpsSGXDevNode->pvISRData);
		if (bStatus)
		{
			OSScheduleMISR((IMG_VOID *)gpsSGXDevNode->psSysData);
		}
		if (gpsSGXDevNode->psSysData->psGlobalEventObject)
		{
			IMG_HANDLE hOSEventKM = gpsSGXDevNode->psSysData->psGlobalEventObject->hOSEventKM;
			if(hOSEventKM)
			{
				OSEventObjectSignalKM(hOSEventKM);
			}
		}
	}

	return bStatus ? 1 : 0;
}
#endif
