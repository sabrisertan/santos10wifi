// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S5d standalone Services-Part1 stage (M4 S5d).
 *
 * Hostile-review execution directive (§13): the exact Services load path
 * is PVRSRVDrmLoad(shell_drm, 0) -> PVRCore_Init() -> exact preamble ->
 * SysInitialise() -> DevInitSGXPart1(). No raw SysInitialise(), no
 * SYSPVRFillCallback/SYSPVRClearCallback (standalone seam skips the
 * obsolete PSB callback table), no SysFinalise, no Part2, no MSI,
 * no SGX MMIO, no DISPLAY writes.
 *
 * Stage control (whole natural boundaries, rmmod/insmod cycles):
 *   santos_stage=0  skeleton: no shell integration, no Services load.
 *   santos_stage=1  resolve shell pointers, print identities, no init.
 *   santos_stage=2  full PVRSRVDrmLoad() through DevInitSGXPart1.
 *
 * Telemetry lives at natural boundaries only; exact sources are NOT
 * edited for prints. PVRSRVDrmLoad rc=0 IS the Part1-PASS signal;
 * on failure the exact PVR_DPF error above is the evidence.
 *
 * Compiled into santos-pvr112.ko (same module as PVRSRVDrmLoad, which
 * is global under SUPPORT_DRI_DRM_EXT). Shell pointers arrive via the
 * shell's read-only exported getters (cross-module dependency).
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/mm.h>

#include <drm/drm_device.h>

#include "img_types.h"
#include "pvr_drm.h"

/* Shell read-only getters (santos-pvr-shell.c, EXPORT_SYMBOL). */
extern struct pci_dev *santos_pvr_shell_pdev(void);
extern struct drm_device *santos_pvr_shell_drm(void);

/* Exact entry points (global under SUPPORT_DRI_DRM_EXT). */
extern IMG_VOID PVRDPFInit(IMG_VOID);

/*
 * S5e0: exact UAPI entry points for the shell module. They are global
 * in this .ko (EXT makes DRI_DRM_STATIC empty) but unexported; export
 * them here (Santos-owned block) so santos-pvr-shell.ko can call the
 * exact handlers directly — no PSB callback table. Exact defining
 * files stay pristine.
 */
extern int PVRSRVDrmOpen(struct drm_device *dev, struct drm_file *file);
extern void PVRSRVDrmPostClose(struct drm_device *dev, struct drm_file *file);
extern int PVRSRV_BridgeDispatchKM(struct drm_device *dev, void *arg,
				   struct drm_file *pFile);
extern int PVRDRM_Dummy_ioctl(struct drm_device *dev, IMG_VOID *arg,
			      struct drm_file *pFile);
extern int PVRDRMIsMaster(struct drm_device *dev, IMG_VOID *arg,
			  struct drm_file *pFile);
extern int PVRDRMUnprivCmd(struct drm_device *dev, IMG_VOID *arg,
			   struct drm_file *pFile);
extern int PVRMMap(struct file *pFile, struct vm_area_struct *ps_vma);
/*
 * MUST be _GPL: shell resolves these at call time via symbol_get, and
 * 7.2 __symbol_get refuses non-GPLONLY symbols outright. Both modules
 * are GPL-compatible (shell "GPL", pvr112 Dual MIT/GPL), so this is safe.
 */
EXPORT_SYMBOL_GPL(PVRSRVDrmOpen);
EXPORT_SYMBOL_GPL(PVRSRVDrmPostClose);
EXPORT_SYMBOL_GPL(PVRSRV_BridgeDispatchKM);
EXPORT_SYMBOL_GPL(PVRDRM_Dummy_ioctl);
EXPORT_SYMBOL_GPL(PVRDRMIsMaster);
EXPORT_SYMBOL_GPL(PVRDRMUnprivCmd);
EXPORT_SYMBOL_GPL(PVRMMap);

/* Own entry points (called from pvr_drm.c module init/exit). */
int santos_s5d_init(void);
void santos_s5d_exit(void);

static int santos_stage;
module_param(santos_stage, int, 0444);
MODULE_PARM_DESC(santos_stage,
		 "S5d stage: 0=skeleton, 1=shell identities, 2=PVRSRVDrmLoad Part1");

/* Gated-cleanup state (§13.4): only Unload what Load came to own. */
static bool s5d_load_owned;
static struct drm_device *s5d_drm_used;

static void santos_s5d_print_identities(struct pci_dev *pdev,
					struct drm_device *drm)
{
	resource_size_t bar0_start = 0, bar0_len = 0;

	if (pdev) {
		bar0_start = pci_resource_start(pdev, 0);
		bar0_len = pci_resource_len(pdev, 0);
	}
	pr_info("santos-s5d: shell pdev=%p (%s) drm=%p\n",
		pdev, pdev ? pci_name(pdev) : "none", drm);
	pr_info("santos-s5d: pre-load BAR0 start=%pa len=%pa irq=%d\n",
		&bar0_start, &bar0_len, pdev ? pdev->irq : -1);
}

int santos_s5d_init(void)
{
	struct pci_dev *pdev;
	struct drm_device *drm;
	int rc;

	if (santos_stage <= 0) {
		pr_info("santos-s5d: stage=0 skeleton, Services load skipped\n");
		return 0;
	}

	pdev = santos_pvr_shell_pdev();
	drm = santos_pvr_shell_drm();
	santos_s5d_print_identities(pdev, drm);

	if (santos_stage == 1) {
		pr_info("santos-s5d: stage=1 identities only, no init\n");
		return 0;
	}

	/* stage >= 2 */
	if (!pdev || !drm) {
		pr_err("santos-s5d: stage=2 needs bound shell (card0), fail cleanly\n");
		return -ENODEV;
	}

	PVRDPFInit();

	/* Standalone seam: SKIP SYSPVRFillCallback (no PSB pvr_ops table). */
	pr_info("santos-s5d: PVRSRVDrmLoad enter (FillCallback skipped)\n");
	rc = PVRSRVDrmLoad(drm, 0);
	if (rc) {
		pr_err("santos-s5d: PVRSRVDrmLoad rc=%d (exact error above is the evidence)\n",
		       rc);
		return rc;
	}

	s5d_load_owned = true;
	s5d_drm_used = drm;
	santos_s5d_print_identities(pdev, drm);
	pr_info("santos-s5d: PVRSRVDrmLoad rc=0 (Services Part1 pre-MMIO alive)\n");
	return 0;
}

void santos_s5d_exit(void)
{
	if (s5d_load_owned && s5d_drm_used) {
		pr_info("santos-s5d: PVRSRVDrmUnload (owned load)\n");
		PVRSRVDrmUnload(s5d_drm_used);
		s5d_load_owned = false;
		s5d_drm_used = NULL;
	} else {
		pr_info("santos-s5d: nothing owned, Unload skipped\n");
	}
	/* NEVER SYSPVRClearCallback in standalone: Fill never ran. */
}
