// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S3 skeleton shim for the Intel PSB display header.
 *
 * sysconfig.c needs struct drm_psb_private (one field) and the
 * IS_MDFLD/IS_MRST/IS_POULSBO selectors from the 1290-line
 * drivers/staging/intel_media/interface/psb_drm.h, which drags
 * TTM placements and old-DRM types. For the no-hardware skeleton
 * we provide the minimum with Santos-fixed answers (Cloverview
 * only). S5 refines this if PSB interop survives.
 */
#ifndef SANTOS_SHIM_PSB_DRM_H
#define SANTOS_SHIM_PSB_DRM_H

struct drm_psb_private {
	int topaz_disabled;
	/* S3: sys_pvr_drm_export.c assigns pvr_ops; full private layout
	 * arrives with PSB interop in S5. */
	struct gpu_pvr_ops *pvr_ops;
};

#define IS_MDFLD(x) (1)
#define IS_MRST(x) (0)
#define IS_POULSBO(x) (0)

#endif /* SANTOS_SHIM_PSB_DRM_H */
