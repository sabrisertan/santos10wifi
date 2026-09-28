// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S5b bind-only PVR DRM shell (M4 S5b) + S5e0 exact UAPI.
 *
 * Owns driver-core binding to 00:02.0 (8086:08c8) and exposes a
 * non-modesetting DRM endpoint (/dev/dri/card0) for the exact-1.12
 * Services to use later. Deliberately does NOT:
 *  - request/map any PCI BAR (Services OSPCI owns resources first);
 *  - enable/reset the device;
 *  - allocate MSI, install IRQ handlers;
 *  - call SysInitialise or any Services init;
 *  - modeset, fbdev, aperture removal, pipe/plane/DPLL/MIPI writes;
 *  - touch simplefb, backlight, or power islands.
 * M3 pre/post fingerprint gates every load/unload cycle.
 * Removal unregisters/unrefs everything it created.
 *
 * S5e0: exact UAPI wired directly (no PSB callback table):
 *  .open=.PVRSRVDrmOpen, .postclose=.PVRSRVDrmPostClose, 4-entry Samsung
 *  ioctl table (golden psb_drv.c numbering), fops.mmap=PVRMMap.
 *  Pre-init open BLOCKS (exact-faithful, killable by signal).
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/file.h>
#include <linux/uaccess.h>

#include <drm/drm_device.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_crtc.h>
#include <drm/drm_plane.h>
#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_helper.h>

#include "img_types.h"
/* Same services include order as pvr_drm.c (header chains need it). */
#include "services.h"
#include "pvr_drm.h"
#include "mmap.h"
#include "pvr_bridge.h"

/* Provided by santos-pvr112 (santos-stub-sync.c); resolved via symbol_get. */
struct file *SantosPVRSyncNewFenceFile(const char *szName);

#define SANTOS_PVR_PCI_VID	0x8086
#define SANTOS_PVR_PCI_DID	0x08c8

/* Exact Samsung bridge ioctl numbers (golden psb_drv.c:348-356). */
#define SANTOS_PVR_SRVKM_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + 0x12, PVRSRV_BRIDGE_PACKAGE)
#define SANTOS_PVR_DISP_IOCTL \
	DRM_IO(DRM_COMMAND_BASE + 0x13)
#define SANTOS_PVR_IS_MASTER_IOCTL \
	DRM_IO(DRM_COMMAND_BASE + 0x15)
#define SANTOS_PVR_UNPRIV_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x16, IMG_UINT32)

/* NOTE: golden DRM_UNLOCKED no longer exists on 7.2 (unlocked is the
 * default); 0 preserves the semantics. DRM_MASTER is still enforced.
 *
 * 7.2 drm_ioctl indexes driver ioctls by nr-DRM_COMMAND_BASE
 * (drm_ioctl.c: index >= num_ioctls -> -EINVAL), so the table MUST be
 * sparse at the real Samsung relative numbers (SRVKM=0x12 etc.), NOT a
 * compact array. Holes (cmd=0, func=NULL) cleanly return -EINVAL;
 * wrong ioctl type returns -ENOTTY (core check before dispatch). */
static int santos_pvr_open(struct drm_device *dev, struct drm_file *file);
static void santos_pvr_postclose(struct drm_device *dev, struct drm_file *file);
static int santos_pvr_srvkm(struct drm_device *dev, void *arg,
			    struct drm_file *file);
static int santos_pvr_disp(struct drm_device *dev, void *arg,
			   struct drm_file *file);
static int santos_pvr_ismaster(struct drm_device *dev, void *arg,
			       struct drm_file *file);
static int santos_pvr_unpriv(struct drm_device *dev, void *arg,
			     struct drm_file *file);
static int santos_pvr_mmap(struct file *filp, struct vm_area_struct *vma);

/*
 * Load-order decoupling: the exact handlers live in santos-pvr112.ko,
 * which itself needs this shell first (card0 + getters). Static linkage
 * both ways would deadlock insmod. Wrappers resolve at call time via
 * symbol_get: either order loads; calls without Services fail clean -ENODEV.
 * STOP: never rmmod santos-pvr112.ko while PVR fds are open (no holder ref).
 */
static int santos_pvr_open(struct drm_device *dev, struct drm_file *file)
{
	int (*fn)(struct drm_device *, struct drm_file *) =
		symbol_get(PVRSRVDrmOpen);
	int r;
	if (!fn)
		return -ENODEV;
	r = fn(dev, file);
	symbol_put(PVRSRVDrmOpen);
	return r;
}

static void santos_pvr_postclose(struct drm_device *dev, struct drm_file *file)
{
	void (*fn)(struct drm_device *, struct drm_file *) =
		symbol_get(PVRSRVDrmPostClose);
	if (!fn) {
		pr_warn("santos-pvr: postclose without Services (leaked perproc)\n");
		return;
	}
	fn(dev, file);
	symbol_put(PVRSRVDrmPostClose);
}

static int santos_pvr_srvkm(struct drm_device *dev, void *arg,
			    struct drm_file *file)
{
	int (*fn)(struct drm_device *, void *, struct drm_file *) =
		symbol_get(PVRSRV_BridgeDispatchKM);
	int r;
	if (!fn)
		return -ENODEV;
	r = fn(dev, arg, file);
	symbol_put(PVRSRV_BridgeDispatchKM);
	return r;
}

static int santos_pvr_disp(struct drm_device *dev, void *arg,
			   struct drm_file *file)
{
	int (*fn)(struct drm_device *, IMG_VOID *, struct drm_file *) =
		symbol_get(PVRDRM_Dummy_ioctl);
	int r;
	if (!fn)
		return -ENODEV;
	r = fn(dev, arg, file);
	symbol_put(PVRDRM_Dummy_ioctl);
	return r;
}

static int santos_pvr_ismaster(struct drm_device *dev, void *arg,
			       struct drm_file *file)
{
	int (*fn)(struct drm_device *, IMG_VOID *, struct drm_file *) =
		symbol_get(PVRDRMIsMaster);
	int r;
	if (!fn)
		return -ENODEV;
	r = fn(dev, arg, file);
	symbol_put(PVRDRMIsMaster);
	return r;
}

static int santos_pvr_unpriv(struct drm_device *dev, void *arg,
			     struct drm_file *file)
{
	int (*fn)(struct drm_device *, IMG_VOID *, struct drm_file *) =
		symbol_get(PVRDRMUnprivCmd);
	int r;
	if (!fn)
		return -ENODEV;
	r = fn(dev, arg, file);
	symbol_put(PVRDRMUnprivCmd);
	return r;
}

static int santos_pvr_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int (*fn)(struct file *, struct vm_area_struct *) =
		symbol_get(PVRMMap);
	int r;
	if (!fn)
		return -ENODEV;
	r = fn(filp, vma);
	symbol_put(PVRMMap);
	return r;
}

/*
 * A140 step 2: minimal PSB extension ioctl (golden psb_drv.c index 6).
 *
 * hwcomposer's libwsbm asks for the TTM extension offsets via
 * drmCommandWriteRead(fd, 6, arg, 128) with extension names such as
 * "psb_ttm_placement_alphadrop"; we advertise the golden command offsets so
 * the next call can be traced. Only the extension registry is implemented
 * here; the TTM/fence commands themselves are separate (and initially
 * unimplemented) table holes.
 */
struct santos_psb_extension_rep {
	int32_t exists;
	uint32_t driver_ioctl_offset;
	uint32_t sarea_offset;
	uint32_t major;
	uint32_t minor;
	uint32_t pl;
};

#define SANTOS_PSB_EXT_NAME_LEN 128

union santos_psb_extension_arg {
	char extension[SANTOS_PSB_EXT_NAME_LEN];
	struct santos_psb_extension_rep rep;
};

#define SANTOS_PSB_EXTENSION_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x06, union santos_psb_extension_arg)

#define SANTOS_PSB_CMDBUF_OFFSET     0x50
#define SANTOS_PSB_SCENE_UNREF_OFFSET 0x51
#define SANTOS_PSB_PLACEMENT_OFFSET  0x52
#define SANTOS_PSB_FENCE_OFFSET      0x59
#define SANTOS_PSB_FLIP_OFFSET       0x5d

static int santos_pvr_psb_extension(struct drm_device *dev, void *arg,
				    struct drm_file *file)
{
	union santos_psb_extension_arg *a = arg;
	uint32_t off = 0;

	(void)dev; (void)file;

	if (!strcmp(a->extension, "psb_ttm_placement_alphadrop"))
		off = SANTOS_PSB_PLACEMENT_OFFSET;
	else if (!strcmp(a->extension, "psb_ttm_fence_alphadrop"))
		off = SANTOS_PSB_FENCE_OFFSET;
	else if (!strcmp(a->extension, "psb_ttm_execbuf_alphadrop"))
		off = SANTOS_PSB_CMDBUF_OFFSET;
	else if (!strcmp(a->extension, "psb_page_flipping_alphadrop"))
		off = SANTOS_PSB_FLIP_OFFSET;

	if (!off) {
		a->rep.exists = 0;
		pr_info("santos-pvr: PSB extension '%s' unknown\n",
			a->extension);
		return 0;
	}

	a->rep.exists = 1;
	a->rep.driver_ioctl_offset = off;
	a->rep.sarea_offset = 0;
	a->rep.major = 1;
	a->rep.minor = 0;
	a->rep.pl = 0;
	pr_info("santos-pvr: PSB extension '%s' -> 0x%x\n", a->extension, off);
	return 0;
}

/*
 * A142: DRM_PSB_GET_DC_INFO (golden psb_drv.c index 0x37).
 *
 * hwcomposer's IntelDisplayPlaneManager asks for the display controller
 * plane inventory before it builds any display device; without this the
 * plane counts stay 0 and display device construction fails. Report the
 * minimal composition inventory: one pipe, one primary plane, no
 * sprite/overlay/cursor planes (all layers fall back to GPU composition).
 */
struct santos_psb_dc_info {
	uint32_t pipe_count;
	uint32_t primary_plane_count;
	uint32_t sprite_plane_count;
	uint32_t overlay_plane_count;
	uint32_t cursor_plane_count;
};

#define SANTOS_PSB_GET_DC_INFO_IOCTL \
	DRM_IOR(DRM_COMMAND_BASE + 0x37, struct santos_psb_dc_info)

static int santos_pvr_psb_dc_info(struct drm_device *dev, void *arg,
				  struct drm_file *file)
{
	struct santos_psb_dc_info *dc = arg;

	(void)dev; (void)file;

	dc->pipe_count = 1;
	dc->primary_plane_count = 1;
	dc->sprite_plane_count = 0;
	dc->overlay_plane_count = 1;
	dc->cursor_plane_count = 0;
	pr_info("santos-pvr: PSB_GET_DC_INFO pipe=1 primary=1 overlay=1\n");
	return 0;
}

/*
 * A146: DRM_PSB_GTT_MAP / _UNMAP (golden psb_drv.c indices 0x0f/0x10).
 *
 * hwcomposer's IntelGraphicBufferManager::gttMap asks the golden PSB GTT
 * allocator for a page offset for a buffer handle. There is no display
 * scanout on 7.2, so hand out synthetic, unique page offsets from a bump
 * allocator; unmap is a no-op. This only has to satisfy HWC's bookkeeping:
 * the returned offset never reaches hardware.
 */
struct santos_psb_gtt_mapping_arg {
	uint32_t type;
	void *hKernelMemInfo;
	uint32_t offset_pages;
	uint32_t page_align;
	uint32_t bcd_device_id;
	uint32_t bcd_buffer_id;
	uint32_t bcd_buffer_count;
	uint32_t bcd_buffer_stride;
	uint32_t vaddr;
	uint32_t size;
};

#define SANTOS_PSB_GTT_MAP_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x0f, struct santos_psb_gtt_mapping_arg)
#define SANTOS_PSB_GTT_UNMAP_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x10, struct santos_psb_gtt_mapping_arg)

static uint32_t santos_pvr_fake_gtt_pages = 0x1000;

static int santos_pvr_psb_gtt_map(struct drm_device *dev, void *arg,
				  struct drm_file *file)
{
	struct santos_psb_gtt_mapping_arg *a = arg;

	(void)dev; (void)file;

	if (a->type != 0)
		return -EINVAL;

	a->offset_pages = santos_pvr_fake_gtt_pages;
	santos_pvr_fake_gtt_pages += 0x100;
	return 0;
}

static int santos_pvr_psb_gtt_unmap(struct drm_device *dev, void *arg,
				    struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	return 0;
}

/*
 * A150: minimal PSB TTM placement subset (golden DRM_PSB_PLACEMENT_OFFSET
 * 0x52..0x58, fence 0x5a..0x5c). hwcomposer's IntelWsbm allocates display
 * buffers (overlay context back buffer) through these ioctls. There is no
 * TTM/GTT backing store in this port, so each create records a fake BO in a
 * small table and hands back synthetic gpu_offset/map_handle values; sync,
 * idle and fence queries all report "ready". Only the data HWC consumes is
 * emulated, and the union sizes match the golden UAPI exactly.
 */
struct santos_ttm_pl_create_req {
	uint64_t size;
	uint32_t placement;
	uint32_t page_alignment;
};

struct santos_ttm_pl_rep {
	uint64_t gpu_offset;
	uint64_t bo_size;
	uint64_t map_handle;
	uint32_t placement;
	uint32_t handle;
	uint32_t sync_object_arg;
	uint32_t pad64;
};

union santos_ttm_pl_create_arg {
	struct santos_ttm_pl_create_req req;
	struct santos_ttm_pl_rep rep;
};

struct santos_ttm_pl_reference_req {
	uint32_t handle;
	uint32_t pad64;
};

union santos_ttm_pl_reference_arg {
	struct santos_ttm_pl_reference_req req;
	struct santos_ttm_pl_rep rep;
};

struct santos_ttm_pl_synccpu_arg {
	uint32_t handle;
	uint32_t access_mode;
	uint32_t op;
	uint32_t pad64;
};

struct santos_ttm_pl_waitidle_arg {
	uint32_t handle;
	uint32_t mode;
};

struct santos_ttm_pl_setstatus_req {
	uint32_t set_placement;
	uint32_t clr_placement;
	uint32_t handle;
	uint32_t pad64;
};

union santos_ttm_pl_setstatus_arg {
	struct santos_ttm_pl_setstatus_req req;
	struct santos_ttm_pl_rep rep;
};

struct santos_ttm_fence_signaled_req {
	uint32_t handle;
	uint32_t fence_type;
	int32_t flush;
	uint32_t pad64;
};

struct santos_ttm_fence_finish_req {
	uint32_t handle;
	uint32_t fence_type;
	uint32_t mode;
	uint32_t pad64;
};

struct santos_ttm_fence_rep {
	uint32_t signaled_types;
	uint32_t fence_error;
};

union santos_ttm_fence_arg {
	struct santos_ttm_fence_signaled_req signaled;
	struct santos_ttm_fence_finish_req finish;
	struct santos_ttm_fence_rep rep;
};

struct santos_ttm_fence_unref_arg {
	uint32_t handle;
	uint32_t pad64;
};

#define SANTOS_TTM_PL_CREATE_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x52, union santos_ttm_pl_create_arg)
#define SANTOS_TTM_PL_REFERENCE_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x53, union santos_ttm_pl_reference_arg)
#define SANTOS_TTM_PL_UNREF_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + 0x54, struct santos_ttm_pl_reference_req)
#define SANTOS_TTM_PL_SYNCCPU_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + 0x55, struct santos_ttm_pl_synccpu_arg)
#define SANTOS_TTM_PL_WAITIDLE_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + 0x56, struct santos_ttm_pl_waitidle_arg)
#define SANTOS_TTM_PL_SETSTATUS_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x57, union santos_ttm_pl_setstatus_arg)
#define SANTOS_TTM_PL_CREATE_UB_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x58, union santos_ttm_pl_create_arg)
#define SANTOS_TTM_FENCE_SIGNALED_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x5a, union santos_ttm_fence_arg)
#define SANTOS_TTM_FENCE_FINISH_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x5b, union santos_ttm_fence_arg)
#define SANTOS_TTM_FENCE_UNREF_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + 0x5c, struct santos_ttm_fence_unref_arg)

#define SANTOS_FAKE_TTM_MAX 64

struct santos_fake_ttm_bo {
	uint32_t handle;
	uint32_t refs;
	uint64_t size;
	uint32_t placement;
	uint64_t gpu_offset;
	uint64_t map_handle;
};

static struct santos_fake_ttm_bo santos_fake_ttm_bos[SANTOS_FAKE_TTM_MAX];
static DEFINE_MUTEX(santos_fake_ttm_lock);
static uint32_t santos_fake_ttm_next = 1;
static uint64_t santos_fake_ttm_gpu_next = 0x10000000;
static uint64_t santos_fake_ttm_map_next = 0x40000000;

static struct santos_fake_ttm_bo *
santos_fake_ttm_find_locked(uint32_t handle)
{
	int i;

	for (i = 0; i < SANTOS_FAKE_TTM_MAX; i++)
		if (santos_fake_ttm_bos[i].handle == handle &&
		    handle != 0)
			return &santos_fake_ttm_bos[i];
	return NULL;
}

static void santos_fake_ttm_fill_rep(struct santos_fake_ttm_bo *bo,
				     struct santos_ttm_pl_rep *rep)
{
	rep->gpu_offset = bo->gpu_offset;
	rep->bo_size = bo->size;
	rep->map_handle = bo->map_handle;
	rep->placement = bo->placement;
	rep->handle = bo->handle;
	rep->sync_object_arg = 0;
	rep->pad64 = 0;
}

static int santos_pvr_psb_pl_create(struct drm_device *dev, void *arg,
				    struct drm_file *file)
{
	union santos_ttm_pl_create_arg *a = arg;
	struct santos_fake_ttm_bo *bo = NULL;
	int i;

	(void)dev; (void)file;

	mutex_lock(&santos_fake_ttm_lock);
	for (i = 0; i < SANTOS_FAKE_TTM_MAX; i++) {
		if (!santos_fake_ttm_bos[i].handle) {
			bo = &santos_fake_ttm_bos[i];
			break;
		}
	}
	if (bo) {
		uint64_t size = a->req.size;

		memset(bo, 0, sizeof(*bo));
		bo->handle = santos_fake_ttm_next++;
		bo->refs = 1;
		bo->size = size;
		bo->placement = a->req.placement;
		bo->gpu_offset = santos_fake_ttm_gpu_next;
		bo->map_handle = santos_fake_ttm_map_next;
		santos_fake_ttm_gpu_next += (size + 0xfff) & ~0xfffULL;
		santos_fake_ttm_map_next += 0x100000;
		santos_fake_ttm_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_fake_ttm_lock);

	if (!bo)
		return -ENOMEM;

	pr_info("santos-pvr: TTM PL_CREATE size=%llu place=0x%x -> handle=%u\n",
		(unsigned long long)a->rep.bo_size, a->rep.placement,
		a->rep.handle);
	return 0;
}

static int santos_pvr_psb_pl_reference(struct drm_device *dev, void *arg,
				       struct drm_file *file)
{
	union santos_ttm_pl_reference_arg *a = arg;
	struct santos_fake_ttm_bo *bo;

	(void)dev; (void)file;

	mutex_lock(&santos_fake_ttm_lock);
	bo = santos_fake_ttm_find_locked(a->req.handle);
	if (bo) {
		bo->refs++;
		santos_fake_ttm_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_fake_ttm_lock);

	return bo ? 0 : -EINVAL;
}

static int santos_pvr_psb_pl_unref(struct drm_device *dev, void *arg,
				   struct drm_file *file)
{
	struct santos_ttm_pl_reference_req *a = arg;
	struct santos_fake_ttm_bo *bo;

	(void)dev; (void)file;

	mutex_lock(&santos_fake_ttm_lock);
	bo = santos_fake_ttm_find_locked(a->handle);
	if (bo && --bo->refs == 0)
		memset(bo, 0, sizeof(*bo));
	mutex_unlock(&santos_fake_ttm_lock);

	return 0;
}

static int santos_pvr_psb_pl_synccpu(struct drm_device *dev, void *arg,
				     struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	return 0;
}

static int santos_pvr_psb_pl_waitidle(struct drm_device *dev, void *arg,
				      struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	return 0;
}

static int santos_pvr_psb_pl_setstatus(struct drm_device *dev, void *arg,
				       struct drm_file *file)
{
	union santos_ttm_pl_setstatus_arg *a = arg;
	struct santos_fake_ttm_bo *bo;

	(void)dev; (void)file;

	mutex_lock(&santos_fake_ttm_lock);
	bo = santos_fake_ttm_find_locked(a->req.handle);
	if (bo) {
		bo->placement |= a->req.set_placement;
		bo->placement &= ~a->req.clr_placement;
		santos_fake_ttm_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_fake_ttm_lock);

	return bo ? 0 : -EINVAL;
}

static int santos_pvr_psb_fence_signaled(struct drm_device *dev, void *arg,
					 struct drm_file *file)
{
	union santos_ttm_fence_arg *a = arg;

	(void)dev; (void)file;

	a->rep.signaled_types = a->signaled.fence_type;
	a->rep.fence_error = 0;
	return 0;
}

static int santos_pvr_psb_fence_finish(struct drm_device *dev, void *arg,
				       struct drm_file *file)
{
	union santos_ttm_fence_arg *a = arg;

	(void)dev; (void)file;

	a->rep.signaled_types = a->finish.fence_type;
	a->rep.fence_error = 0;
	return 0;
}

static int santos_pvr_psb_fence_unref(struct drm_device *dev, void *arg,
				      struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	return 0;
}

static const struct drm_ioctl_desc santos_pvr_ioctls[] = {
	[0x06] = { .cmd = SANTOS_PSB_EXTENSION_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_extension,
	  .name = "PSB_EXTENSION" },
	[0x0f] = { .cmd = SANTOS_PSB_GTT_MAP_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_gtt_map,
	  .name = "PSB_GTT_MAP" },
	[0x10] = { .cmd = SANTOS_PSB_GTT_UNMAP_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_gtt_unmap,
	  .name = "PSB_GTT_UNMAP" },
	[0x37] = { .cmd = SANTOS_PSB_GET_DC_INFO_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_dc_info,
	  .name = "PSB_GET_DC_INFO" },
	[0x52] = { .cmd = SANTOS_TTM_PL_CREATE_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_pl_create,
	  .name = "TTM_PL_CREATE" },
	[0x53] = { .cmd = SANTOS_TTM_PL_REFERENCE_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_pl_reference,
	  .name = "TTM_PL_REFERENCE" },
	[0x54] = { .cmd = SANTOS_TTM_PL_UNREF_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_pl_unref,
	  .name = "TTM_PL_UNREF" },
	[0x55] = { .cmd = SANTOS_TTM_PL_SYNCCPU_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_pl_synccpu,
	  .name = "TTM_PL_SYNCCPU" },
	[0x56] = { .cmd = SANTOS_TTM_PL_WAITIDLE_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_pl_waitidle,
	  .name = "TTM_PL_WAITIDLE" },
	[0x57] = { .cmd = SANTOS_TTM_PL_SETSTATUS_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_pl_setstatus,
	  .name = "TTM_PL_SETSTATUS" },
	[0x58] = { .cmd = SANTOS_TTM_PL_CREATE_UB_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_pl_create,
	  .name = "TTM_PL_CREATE_UB" },
	[0x5a] = { .cmd = SANTOS_TTM_FENCE_SIGNALED_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_fence_signaled,
	  .name = "TTM_FENCE_SIGNALED" },
	[0x5b] = { .cmd = SANTOS_TTM_FENCE_FINISH_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_fence_finish,
	  .name = "TTM_FENCE_FINISH" },
	[0x5c] = { .cmd = SANTOS_TTM_FENCE_UNREF_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_fence_unref,
	  .name = "TTM_FENCE_UNREF" },
	[0x12] = { .cmd = SANTOS_PVR_SRVKM_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_srvkm,
	  .name = "PVR_SRVKM" },
	[0x13] = { .cmd = SANTOS_PVR_DISP_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_disp,
	  .name = "PVR_DISP" },
	[0x15] = { .cmd = SANTOS_PVR_IS_MASTER_IOCTL,
	  .flags = DRM_MASTER,
	  .func = santos_pvr_ismaster,
	  .name = "PVR_IS_MASTER" },
	[0x16] = { .cmd = SANTOS_PVR_UNPRIV_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_unpriv,
	  .name = "PVR_UNPRIV" },
};
static_assert(ARRAY_SIZE(santos_pvr_ioctls) == 0x5d);

static struct pci_dev *santos_pvr_pdev;
static struct drm_device *santos_pvr_drm;

/*
 * S5d read-only getters for the Services module (hostile-review §13.1).
 * Exporting these creates an explicit module dependency when
 * santos-pvr112.ko references them. No writable pointers exported.
 */
struct pci_dev *santos_pvr_shell_pdev(void);
struct drm_device *santos_pvr_shell_drm(void);

struct pci_dev *santos_pvr_shell_pdev(void)
{
	return santos_pvr_pdev;
}
EXPORT_SYMBOL(santos_pvr_shell_pdev);

struct drm_device *santos_pvr_shell_drm(void)
{
	return santos_pvr_drm;
}
EXPORT_SYMBOL(santos_pvr_shell_drm);

static const struct file_operations santos_pvr_fops = {
	.owner		= THIS_MODULE,
	.fop_flags	= FOP_UNSIGNED_OFFSET,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
	/* no .compat_ioctl (i386 native; S5f if ever needed) */
	.poll		= drm_poll,
	.read		= drm_read,
	.mmap		= santos_pvr_mmap,
	.llseek		= noop_llseek,
};

/*
 * Minimal headless KMS stub (A140).
 *
 * hwcomposer.clovertrail opens the same /dev/card0 (not /dev/dri/card0) and
 * requires DRM_IOCTL_MODE_GETRESOURCES/GETCONNECTOR to succeed with a
 * 1280x800 mode before the eglfs_santos Qt plugin can create its native
 * window. The 7.2 port has no display driver, so expose one fake
 * plane/crtc/encoder/connector with a fixed mode; nothing is scanout
 * capable and set_config is a no-op bookkeeping update.
 */
#define SANTOS_FAKE_HW 1280
#define SANTOS_FAKE_VW 800

static struct drm_plane santos_fake_plane;
static struct drm_crtc santos_fake_crtc;
static struct drm_encoder santos_fake_encoder;
static struct drm_connector santos_fake_connector;

/* 1280x800@60 CVT-ish; only the fields libdrm reports to userspace matter. */
static const struct drm_display_mode santos_fake_mode = {
	.clock = 83500,
	.hdisplay = SANTOS_FAKE_HW,
	.hsync_start = SANTOS_FAKE_HW + 72,
	.hsync_end = SANTOS_FAKE_HW + 160,
	.htotal = SANTOS_FAKE_HW + 400,
	.vdisplay = SANTOS_FAKE_VW,
	.vsync_start = SANTOS_FAKE_VW + 3,
	.vsync_end = SANTOS_FAKE_VW + 9,
	.vtotal = SANTOS_FAKE_VW + 28,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int santos_fake_fill_modes(struct drm_connector *connector,
				  uint32_t max_width, uint32_t max_height)
{
	struct drm_display_mode *mode;
	(void)max_width; (void)max_height;

	if (!list_empty(&connector->modes) ||
	    !list_empty(&connector->probed_modes))
		return 1;

	mode = drm_mode_duplicate(connector->dev, &santos_fake_mode);
	if (!mode)
		return 0;
	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_connector_funcs santos_fake_connector_funcs = {
	.fill_modes = santos_fake_fill_modes,
	.destroy = drm_connector_cleanup,
};

static int santos_fake_set_config(struct drm_mode_set *set,
				  struct drm_modeset_acquire_ctx *ctx)
{
	(void)ctx;
	if (set->crtc == NULL)
		return -EINVAL;
	set->crtc->x = set->x;
	set->crtc->y = set->y;
	if (set->mode) {
		drm_mode_copy(&set->crtc->mode, set->mode);
		set->crtc->enabled = true;
	} else {
		set->crtc->enabled = false;
	}
	return 0;
}

static const struct drm_crtc_funcs santos_fake_crtc_funcs = {
	.set_config = santos_fake_set_config,
	.destroy = drm_crtc_cleanup,
};

static int santos_fake_update_plane(struct drm_plane *plane,
				    struct drm_crtc *crtc,
				    struct drm_framebuffer *fb,
				    int crtc_x, int crtc_y,
				    unsigned int crtc_w, unsigned int crtc_h,
				    uint32_t src_x, uint32_t src_y,
				    uint32_t src_w, uint32_t src_h,
				    struct drm_modeset_acquire_ctx *ctx)
{
	(void)plane; (void)crtc; (void)fb; (void)crtc_x; (void)crtc_y;
	(void)crtc_w; (void)crtc_h; (void)src_x; (void)src_y;
	(void)src_w; (void)src_h; (void)ctx;
	return 0;
}

static int santos_fake_disable_plane(struct drm_plane *plane,
				     struct drm_modeset_acquire_ctx *ctx)
{
	(void)plane; (void)ctx;
	return 0;
}

static const struct drm_plane_funcs santos_fake_plane_funcs = {
	.update_plane = santos_fake_update_plane,
	.disable_plane = santos_fake_disable_plane,
	.destroy = drm_plane_cleanup,
};

static const struct drm_encoder_funcs santos_fake_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

/*
 * A143: unbacked framebuffer plumbing.
 *
 * hwcomposer detects an output by walking connector->encoder->crtc and
 * then requires drmModeGetFB(crtc->buffer_id) to succeed; a CRTC without
 * an fb is marked disconnected and display configs fail. It also wraps
 * every gralloc buffer with legacy drmModeAddFB. Nothing is scanned out
 * on 7.2, so the shell owns drm_framebuffer objects with no GEM backing:
 * fb_create accepts any handle, and create_handle feeds that handle back
 * out on GETFB.
 */
struct santos_fake_fb {
	struct drm_framebuffer base;
	uint32_t handle;
};

static int santos_fake_fb_create_handle(struct drm_framebuffer *fb,
					struct drm_file *file_priv,
					unsigned int *handle)
{
	struct santos_fake_fb *s =
		container_of(fb, struct santos_fake_fb, base);

	(void)file_priv;
	*handle = s->handle;
	return 0;
}

static void santos_fake_fb_destroy(struct drm_framebuffer *fb)
{
	struct santos_fake_fb *s =
		container_of(fb, struct santos_fake_fb, base);

	drm_framebuffer_cleanup(fb);
	kfree(s);
}

static const struct drm_framebuffer_funcs santos_fake_fb_funcs = {
	.destroy = santos_fake_fb_destroy,
	.create_handle = santos_fake_fb_create_handle,
};

static void santos_fake_boot_fb_destroy(struct drm_framebuffer *fb)
{
	drm_framebuffer_cleanup(fb);
}

static const struct drm_framebuffer_funcs santos_fake_boot_fb_funcs = {
	.destroy = santos_fake_boot_fb_destroy,
	.create_handle = santos_fake_fb_create_handle,
};

static struct santos_fake_fb santos_fake_boot_fb;

static struct drm_framebuffer *
santos_fake_fb_create(struct drm_device *dev, struct drm_file *file_priv,
		      const struct drm_format_info *info,
		      const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct santos_fake_fb *s;
	int ret;

	(void)file_priv;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return ERR_PTR(-ENOMEM);

	drm_helper_mode_fill_fb_struct(dev, &s->base, info, mode_cmd);
	s->handle = mode_cmd->handles[0];

	ret = drm_framebuffer_init(dev, &s->base, &santos_fake_fb_funcs);
	if (ret) {
		kfree(s);
		return ERR_PTR(ret);
	}

	pr_info("santos-pvr: fake fb %ux%u id=%u handle=0x%x\n",
		s->base.width, s->base.height, s->base.base.id, s->handle);
	return &s->base;
}

static const struct drm_mode_config_funcs santos_fake_mode_config_funcs = {
	.fb_create = santos_fake_fb_create,
};

static int santos_pvr_kms_init(struct drm_device *dev)
{
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_width = SANTOS_FAKE_HW;
	dev->mode_config.max_height = SANTOS_FAKE_VW;
	dev->mode_config.preferred_depth = 24;
	dev->mode_config.prefer_shadow = 0;
	dev->mode_config.funcs = &santos_fake_mode_config_funcs;

	{
		static const uint32_t santos_fake_formats[] = {
			DRM_FORMAT_XRGB8888,
			DRM_FORMAT_ARGB8888,
		};
		ret = drm_universal_plane_init(dev, &santos_fake_plane, 1,
					       &santos_fake_plane_funcs,
					       santos_fake_formats,
					       ARRAY_SIZE(santos_fake_formats),
					       NULL, DRM_PLANE_TYPE_PRIMARY,
					       "santos-fake-plane");
	}
	if (ret)
		return ret;

	ret = drm_crtc_init_with_planes(dev, &santos_fake_crtc,
					&santos_fake_plane, NULL,
					&santos_fake_crtc_funcs,
					"santos-fake-crtc");
	if (ret)
		return ret;

	/* Boot framebuffer: hwcomposer requires a valid GETFB on the CRTC. */
	{
		struct drm_mode_fb_cmd2 cmd = {
			.width = SANTOS_FAKE_HW,
			.height = SANTOS_FAKE_VW,
			.pixel_format = DRM_FORMAT_XRGB8888,
			.pitches = { SANTOS_FAKE_HW * 4 },
		};
		const struct drm_format_info *info =
			drm_get_format_info(dev, cmd.pixel_format, 0);

		if (!info)
			return -EINVAL;
		santos_fake_boot_fb.handle = 0x5a170001;
		drm_helper_mode_fill_fb_struct(dev, &santos_fake_boot_fb.base,
					       info, &cmd);
		ret = drm_framebuffer_init(dev, &santos_fake_boot_fb.base,
					   &santos_fake_boot_fb_funcs);
		if (ret)
			return ret;
		santos_fake_plane.fb = &santos_fake_boot_fb.base;
		santos_fake_plane.crtc = &santos_fake_crtc;
	}

	ret = drm_encoder_init(dev, &santos_fake_encoder,
			       &santos_fake_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, "santos-fake-enc");
	if (ret)
		return ret;
	santos_fake_encoder.possible_crtcs = 1;

	ret = drm_connector_init(dev, &santos_fake_connector,
				 &santos_fake_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	drm_connector_attach_encoder(&santos_fake_connector,
				     &santos_fake_encoder);

	/*
	 * hwcomposer.clovertrail looks for the primary panel on display 0 as
	 * DRM_MODE_CONNECTOR_VIRTUAL with connector_type_id 1, then follows
	 * connector->encoder_id -> encoder->crtc_id and requires the CRTC to
	 * report a valid mode. Non-atomic drivers expose the legacy
	 * connector->encoder / encoder->crtc pointers, so wire that chain up
	 * by hand (the core leaves it NULL until the first modeset).
	 */
	santos_fake_connector.connector_type_id = 1;
	santos_fake_connector.encoder = &santos_fake_encoder;
	santos_fake_encoder.crtc = &santos_fake_crtc;
	santos_fake_crtc.enabled = true;
	drm_mode_copy(&santos_fake_crtc.mode, &santos_fake_mode);

	santos_fake_connector.status = connector_status_connected;
	santos_fake_connector.display_info.width_mm = 217;
	santos_fake_connector.display_info.height_mm = 135;

	/* Also make the mode available to the first GETCONNECTOR probe. */
	mutex_lock(&dev->mode_config.mutex);
	santos_fake_fill_modes(&santos_fake_connector, SANTOS_FAKE_HW,
			       SANTOS_FAKE_VW);
	mutex_unlock(&dev->mode_config.mutex);

	pr_info("santos-pvr: KMS stub registered (fake %dx%d connector)\n",
		SANTOS_FAKE_HW, SANTOS_FAKE_VW);
	return 0;
}

static const struct drm_driver santos_pvr_drm_driver = {
	.driver_features	= DRIVER_MODESET,
	.name			= "santos-pvr",
	.desc			= "Santos Cloverview PVR DRM shell (S5e0 exact UAPI)",
	.major			= 1,
	.minor			= 0,
	.patchlevel		= 0,
	.fops			= &santos_pvr_fops,
	.open			= santos_pvr_open,
	.postclose		= santos_pvr_postclose,
	.ioctls			= santos_pvr_ioctls,
	.num_ioctls		= ARRAY_SIZE(santos_pvr_ioctls),
};

static int santos_pvr_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct drm_device *drm;
	int ret;

	pr_info("santos-pvr: probe 00:02.0 (no BAR claim, no reset)\n");

	drm = drm_dev_alloc(&santos_pvr_drm_driver, &pdev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	ret = santos_pvr_kms_init(drm);
	if (ret) {
		pr_err("santos-pvr: KMS stub init failed: %d\n", ret);
		drm_dev_put(drm);
		return ret;
	}

	pci_set_drvdata(pdev, drm);
	santos_pvr_pdev = pdev;
	santos_pvr_drm = drm;

	ret = drm_dev_register(drm, 0);
	if (ret) {
		pci_set_drvdata(pdev, NULL);
		santos_pvr_pdev = NULL;
		santos_pvr_drm = NULL;
		drm_dev_put(drm);
		return ret;
	}

	pr_info("santos-pvr: card registered, M3 untouched\n");
	return 0;
}

static void santos_pvr_remove(struct pci_dev *pdev)
{
	struct drm_device *drm = pci_get_drvdata(pdev);

	if (!drm)
		return;
	drm_dev_unplug(drm);
	drm_dev_unregister(drm);
	pci_set_drvdata(pdev, NULL);
	santos_pvr_pdev = NULL;
	santos_pvr_drm = NULL;
	drm_dev_put(drm);
	pr_info("santos-pvr: removed\n");
}

static const struct pci_device_id santos_pvr_ids[] = {
	{ PCI_DEVICE(SANTOS_PVR_PCI_VID, SANTOS_PVR_PCI_DID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, santos_pvr_ids);

static struct pci_driver santos_pvr_pci_driver = {
	.name		= "santos-pvr-shell",
	.id_table	= santos_pvr_ids,
	.probe		= santos_pvr_probe,
	.remove		= santos_pvr_remove,
};

/*
 * A144: minimal /dev/pvr_sync character device.
 *
 * gralloc.clovertrail.so opens /dev/pvr_sync and drives the PVR sync
 * protocol from its createfence path: PVR_SYNC_IOC_ALLOC_FENCE allocates
 * a timeline handle, then PVR_SYNC_IOC_CREATE_FENCE derives a waitable
 * fence fd from it. The golden implementation backs these with SGX kick
 * sync objects (pvrsync.c, old Android sync_timeline API); this port has
 * no GPU sync-object plumbing yet, and nothing is scanned out, so every
 * requested fence is reported as an already-signaled dma-fence stub via
 * sync_file. Waits succeed immediately and gralloc can finish the buffer
 * handoff; the real kick-attached fences land with the S5 dma_fence port.
 */
struct santos_pvr_sync_alloc {
	__s32 fence;
	__u32 bTimelineIdle;
};

struct santos_pvr_sync_create {
	char name[32];
	__s32 allocdSyncInfo;
	__s32 fence;
};

#define SANTOS_PVR_SYNC_IOC_CREATE_FENCE \
	_IOWR('W', 0, struct santos_pvr_sync_create)
#define SANTOS_PVR_SYNC_IOC_ALLOC_FENCE \
	_IOWR('W', 2, struct santos_pvr_sync_alloc)

static struct file *santos_pvr_sync_new_fence(const char *name)
{
	struct file *(*fn)(const char *) =
		symbol_get(SantosPVRSyncNewFenceFile);
	struct file *f;

	if (!fn)
		return ERR_PTR(-ENODEV);

	f = fn(name);
	symbol_put(SantosPVRSyncNewFenceFile);
	return f;
}

static long santos_pvr_sync_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	(void)file;

	switch (cmd) {
	case SANTOS_PVR_SYNC_IOC_ALLOC_FENCE: {
		struct santos_pvr_sync_alloc data;
		struct file *f;
		int fd;

		f = santos_pvr_sync_new_fence("pvr_sync_alloc");
		if (IS_ERR(f))
			return PTR_ERR(f);
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			fput(f);
			return fd;
		}
		data.fence = fd;
		data.bTimelineIdle = 1;
		if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
			fput(f);
			put_unused_fd(fd);
			return -EFAULT;
		}
		fd_install(fd, f);
		return 0;
	}
	case SANTOS_PVR_SYNC_IOC_CREATE_FENCE: {
		struct santos_pvr_sync_create data;
		struct file *f;
		int fd;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;
		f = santos_pvr_sync_new_fence(data.name);
		if (IS_ERR(f))
			return PTR_ERR(f);
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			fput(f);
			return fd;
		}
		data.fence = fd;
		if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
			fput(f);
			put_unused_fd(fd);
			return -EFAULT;
		}
		fd_install(fd, f);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations santos_pvr_sync_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = santos_pvr_sync_ioctl,
	.compat_ioctl = santos_pvr_sync_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice santos_pvr_sync_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pvr_sync",
	.fops = &santos_pvr_sync_fops,
	.mode = 0666,
};

static int __init santos_pvr_shell_init(void)
{
	int ret;

	pr_info("santos-pvr-shell: loading (bind-only, no hardware init)\n");
	ret = misc_register(&santos_pvr_sync_miscdev);
	if (ret) {
		pr_err("santos-pvr-shell: pvr_sync misc_register failed: %d\n",
		       ret);
		return ret;
	}
	ret = pci_register_driver(&santos_pvr_pci_driver);
	if (ret)
		misc_deregister(&santos_pvr_sync_miscdev);
	return ret;
}

static void __exit santos_pvr_shell_exit(void)
{
	pci_unregister_driver(&santos_pvr_pci_driver);
	misc_deregister(&santos_pvr_sync_miscdev);
	santos_pvr_pdev = NULL;
	santos_pvr_drm = NULL;
	pr_info("santos-pvr-shell: unloaded\n");
}

module_init(santos_pvr_shell_init);
module_exit(santos_pvr_shell_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("Bind-only PVR DRM shell for Cloverview 00:02.0 (M4 S5b)");
MODULE_LICENSE("GPL");
