// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S3 skeleton shim for psb_drv.h (Intel PSB private header).
 *
 * sysconfig.c / sys_pvr_drm_export.c need struct gpu_pvr_ops (copied
 * VERBATIM below from common/psb_drv.h — same member order/types, so
 * layout-identical) and the IS_MDFLD/IS_MRST/IS_POULSBO selectors.
 * The real 1399-line header drags TTM + old-DRM worlds; Santos is
 * Cloverview-only so the selectors are fixed answers. S5 refines if
 * PSB interop survives. Skeleton links, never runs.
 */
#ifndef SANTOS_SHIM_PSB_DRV_H
#define SANTOS_SHIM_PSB_DRV_H

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include "psb_powermgmt.h"

/* Fixed answers for a Cloverview-only build. */
#define IS_MDFLD(x) (1)
#define IS_MRST(x) (0)
#define IS_POULSBO(x) (0)

/* Verbatim from common/psb_drv.h (member order/types preserved). */
struct gpu_pvr_ops {
	IMG_BOOL (*PVRGetDisplayClassJTable)(PVRSRV_DC_DISP2SRV_KMJTABLE
		*psJTable);
#if defined(SUPPORT_DRI_DRM_EXT)
	int (*SYSPVRServiceSGXInterrupt)(struct drm_device *dev);
#endif
	int (*PVRSRVDrmLoad)(struct drm_device *dev, unsigned long flags);
	int (*SYSPVRInit)(void);
	int (*PVRDRM_Dummy_ioctl)(struct drm_device *dev, void *arg,
			struct drm_file *pFile);
	int (*PVRMMap)(struct file *pFile, struct vm_area_struct *ps_vma);
	void (*PVRSRVDrmPostClose)(struct drm_device *dev,
			struct drm_file *file);
	int (*PVRSRV_BridgeDispatchKM)(struct drm_device unref__ * dev,
			void *arg, struct drm_file *pFile);
	int (*PVRSRVOpen)(struct drm_device unref__ *dev,
			struct drm_file *pFile);
	int (*PVRDRMIsMaster)(struct drm_device *dev, void *arg,
			struct drm_file *pFile);
	int (*PVRDRMUnprivCmd)(struct drm_device *dev, void *arg,
			struct drm_file *pFile);
	int (*SYSPVRDBGDrivIoctl)(struct drm_device *dev, IMG_VOID *arg,
			struct drm_file *pFile);
	int (*PVRSRVDrmUnload)(struct drm_device *dev);
	PVRSRV_PER_PROCESS_DATA *(*PVRSRVPerProcessData)(IMG_UINT32 ui32PID);
#if defined (SUPPORT_SID_INTERFACE)
	PVRSRV_ERROR (*PVRSRVLookupHandle)(PVRSRV_HANDLE_BASE *psBase,
			IMG_PVOID *ppvData, IMG_SID hHandle,
			PVRSRV_HANDLE_TYPE eType);
#else
	PVRSRV_ERROR (*PVRSRVLookupHandle)(PVRSRV_HANDLE_BASE *psBase,
			IMG_PVOID *ppvData, IMG_HANDLE hHandle,
			PVRSRV_HANDLE_TYPE eType);
#endif
	IMG_CPU_PHYADDR (*LinuxMemAreaToCpuPAddr)(LinuxMemArea *psLinuxMemArea,
			IMG_UINT32 ui32ByteOffset);
	PVRSRV_ERROR (*OSScheduleMISR2)(void);
};

#endif /* SANTOS_SHIM_PSB_DRV_H */
