// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S5b bind-only PVR DRM shell (M4 S5b) + S5e0 exact UAPI.
 *
 * Owns driver-core binding to 00:02.0 (8086:08c8) and exposes a
 * non-modesetting DRM endpoint (/dev/dri/card0) for the exact-1.12
 * Services to use later. Deliberately does NOT:
 *  - request/map any PCI BAR (Services OSPCI owns resources first);
 *  - enable/reset the device;
 * MSI/IRQ is attached only by Services after its MISR is ready.
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
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/workqueue.h>

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
#include <asm/cacheflush.h>
#include "services.h"
#include "pvr_drm.h"
#include "mmap.h"
#include "pvr_bridge.h"
#include "santos-irq.h"
#include "santos-vdx-memory.h"
#include "santos-vdx-irq.h"

/* Provided by santos-pvr112 (santos-stub-sync.c); resolved via symbol_get. */
struct file *SantosPVRSyncNewFenceFile(const char *szName);

#define SANTOS_PVR_PCI_VID	0x8086
#define SANTOS_PVR_PCI_DID	0x08c8

/* Exact Clovertrail PSB VSYNC ABI (golden psb_drm.h). */
#define SANTOS_PSB_VSYNC_ENABLE		(1U << 0)
#define SANTOS_PSB_VSYNC_DISABLE	(1U << 1)
#define SANTOS_PSB_VSYNC_WAIT		(1U << 2)
#define SANTOS_PSB_GET_VSYNC_COUNT	(1U << 3)
#define SANTOS_PSB_VSYNC_VALID_MASK	(SANTOS_PSB_VSYNC_ENABLE | \
					 SANTOS_PSB_VSYNC_DISABLE | \
					 SANTOS_PSB_VSYNC_WAIT | \
					 SANTOS_PSB_GET_VSYNC_COUNT)
#define SANTOS_PSB_VSYNC_PIPE_A		0U

/* Exact Pipe-A frame-counter registers relative to Services display aperture (BAR0 + 0x70000). */
#define SANTOS_VDC_PIPEACONF		0x00008U
#define SANTOS_VDC_PIPEAFRAMEHIGH	0x00040U
#define SANTOS_VDC_PIPEAFRAMEPIXEL	0x00044U
#define SANTOS_PIPEACONF_ENABLE		BIT(31)
#define SANTOS_PIPE_FRAME_HIGH_MASK	0x0000ffffU
#define SANTOS_PIPE_FRAME_HIGH_SHIFT	0
#define SANTOS_PIPE_FRAME_LOW_MASK	0xff000000U
#define SANTOS_PIPE_FRAME_LOW_SHIFT	24

/* Golden DRM_HZ is the kernel HZ; the current kernel no longer exports the
 * old DRM_HZ spelling. Polling is only used because this shell has no display
 * vblank IRQ path; it is a software observation of a proven real frame-counter
 * transition, not a hardware-captured presentation timestamp or presentation-
 * completion signal. Timestamps use system CLOCK_MONOTONIC nanoseconds via
 * ktime_get_ns() to satisfy the HWC1 vsync callback contract, an intentional
 * standards correction over golden kernel 3.4's getrawmonotonic(). */
#define SANTOS_PSB_VSYNC_TIMEOUT_JIFFIES	(3 * HZ)
#define SANTOS_PSB_VSYNC_POLL_NS		(2 * NSEC_PER_MSEC)

struct santos_psb_vsync_set_arg {
	uint32_t vsync_operation_mask;
	struct {
		uint32_t pipe;
		int32_t vsync_pipe;
		int32_t vsync_count;
		uint64_t timestamp;
	} vsync;
};

static_assert(sizeof(struct santos_psb_vsync_set_arg) == 24);
static_assert(offsetof(struct santos_psb_vsync_set_arg,
			   vsync_operation_mask) == 0);
static_assert(offsetof(struct santos_psb_vsync_set_arg, vsync.pipe) == 4);
static_assert(offsetof(struct santos_psb_vsync_set_arg,
			   vsync.vsync_pipe) == 8);
static_assert(offsetof(struct santos_psb_vsync_set_arg,
			   vsync.vsync_count) == 12);
static_assert(offsetof(struct santos_psb_vsync_set_arg,
			   vsync.timestamp) == 16);

#define SANTOS_PSB_VSYNC_SET_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + 0x32, struct santos_psb_vsync_set_arg)

static_assert(_IOC_SIZE(SANTOS_PSB_VSYNC_SET_IOCTL) == 24);
static_assert(DRM_IOCTL_NR(SANTOS_PSB_VSYNC_SET_IOCTL) ==
		      DRM_COMMAND_BASE + 0x32);

static DEFINE_MUTEX(santos_irq_lock);
static void __iomem *santos_irq_regs;
static void __iomem *santos_vsync_regs;
static int (*santos_irq_service)(struct drm_device *);
static int santos_irq = -1;
static bool santos_irq_stopping;
static void (*santos_vdx_irq_service)(void);
static void (*santos_vdx_irq_stopped)(void);
static int (*santos_vdx_irq_provider_pin)(struct drm_device *, struct drm_file *);
static u32 santos_vdx_irq_saved_enable, santos_vdx_irq_saved_mask;

/* VSYNC state is deliberately separate from Candidate3B retirement state. */
static DEFINE_MUTEX(santos_vsync_state_lock);
static DECLARE_WAIT_QUEUE_HEAD(santos_vsync_waitq);
static DECLARE_WAIT_QUEUE_HEAD(santos_vsync_users_wq);
static atomic_t santos_vsync_users = ATOMIC_INIT(0);
static bool santos_vsync_enabled;
static unsigned long santos_vsync_epoch;

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
static void santos_vdx_file_close(struct file *filp);
static void santos_pvr_postclose(struct drm_device *dev, struct drm_file *file);
static int santos_pvr_srvkm(struct drm_device *dev, void *arg,
			    struct drm_file *file);
static int santos_pvr_disp(struct drm_device *dev, void *arg,
			   struct drm_file *file);
static int santos_pvr_ismaster(struct drm_device *dev, void *arg,
			       struct drm_file *file);
static int santos_pvr_unpriv(struct drm_device *dev, void *arg,
			     struct drm_file *file);
static int santos_vdx_bo_mmap(struct vm_area_struct *vma);
static int santos_pvr_mmap(struct file *filp, struct vm_area_struct *vma);
static int santos_pvr_psb_vsync_set(struct drm_device *dev, void *arg,
				    struct drm_file *file);

/*
 * Load-order decoupling: the exact handlers live in santos-pvr112.ko,
 * which itself needs this shell first (card0 + getters). Static linkage
 * both ways would deadlock insmod. Wrappers resolve at call time via
 * symbol_get: either order loads; calls without Services fail clean -ENODEV.
 * Successful opens retain a Services reference until postclose.
 */
static int santos_pvr_open(struct drm_device *dev, struct drm_file *file)
{
	int (*fn)(struct drm_device *, struct drm_file *) =
		symbol_get(PVRSRVDrmOpen);
	int r;
	if (!fn)
		return -ENODEV;
	r = fn(dev, file);
	/* Keep Services alive for this file, including its mmap lifetime. */
	if (r)
		symbol_put(PVRSRVDrmOpen);
	return r;
}

static void santos_pvr_postclose(struct drm_device *dev, struct drm_file *file)
{
	void (*fn)(struct drm_device *, struct drm_file *) =
		symbol_get(PVRSRVDrmPostClose);
	santos_vdx_file_close(file->filp);
	if (!fn) {
		pr_warn("santos-pvr: postclose without Services (leaked perproc)\n");
		return;
	}
	fn(dev, file);
	symbol_put(PVRSRVDrmPostClose);
	symbol_put(PVRSRVDrmOpen);
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
	int (*fn)(struct file *, struct vm_area_struct *);
	int r;

	r = santos_vdx_bo_mmap(vma);
	if (r != -ENOENT)
		return r;

	fn = symbol_get(PVRMMap);
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
#define SANTOS_PSB_GETPARAM          0x5e

static int santos_pvr_psb_extension(struct drm_device *dev, void *arg,
				    struct drm_file *file)
{
	union santos_psb_extension_arg *a = arg;
	uint32_t off = 0;

	(void)dev; (void)file;

	if (!memchr(a->extension, '\0', sizeof(a->extension)))
		return -EINVAL;
	if (!strcmp(a->extension, "psb_ttm_placement_alphadrop"))
		off = SANTOS_PSB_PLACEMENT_OFFSET;
	else if (!strcmp(a->extension, "psb_ttm_fence_alphadrop"))
		off = SANTOS_PSB_FENCE_OFFSET;
	else if (!strcmp(a->extension, "psb_ttm_execbuf_alphadrop"))
		off = SANTOS_PSB_CMDBUF_OFFSET;
	else if (!strcmp(a->extension, "psb_page_flipping_alphadrop"))
		off = SANTOS_PSB_FLIP_OFFSET;
	else if (!strcmp(a->extension, "lnc_video_getparam"))
		off = SANTOS_PSB_GETPARAM;

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
	pr_info("santos-pvr: PSB extension resolved -> 0x%x\n", off);
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
 *
 * overlay_plane_count MUST stay 0: the kernel has no complete overlay
 * support (no DRM_PSB_REGISTER_RW handler, synthetic GTT offsets) and the
 * live HWC is the A148-clean binary whose setZOrderConfig is a deliberate
 * no-op written for the overlay=0 inventory. Reporting overlay=1 made HWC
 * enter an overlay initialization it cannot finish; the A149/A150
 * experiments were rejected and reverted for exactly this reason.
 * See notes/fake-kms-psb-audit-20260916/.
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
	dc->overlay_plane_count = 0;
	dc->cursor_plane_count = 0;
	pr_info("santos-pvr: PSB_GET_DC_INFO pipe=1 primary=1 overlay=0\n");
	return 0;
}

/*
 * A146: DRM_PSB_GTT_MAP / _UNMAP (golden psb_drv.c indices 0x0f/0x10).
 *
 * hwcomposer's IntelGraphicBufferManager::gttMap asks the golden PSB GTT
 * allocator for a page offset for a buffer handle. There is no display
 * scanout on 7.2, so hand out synthetic page offsets from an atomic
 * monotonic counter; unmap is a no-op. This only has to satisfy HWC's
 * bookkeeping: the returned offset never reaches hardware.
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

static atomic_t santos_pvr_fake_gtt_pages = ATOMIC_INIT(0x1000);

static int santos_pvr_psb_gtt_map(struct drm_device *dev, void *arg,
				  struct drm_file *file)
{
	struct santos_psb_gtt_mapping_arg *a = arg;

	(void)dev; (void)file;

	if (a->type != 0)
		return -EINVAL;

	a->offset_pages = (uint32_t)atomic_fetch_add_relaxed(
				0x100, &santos_pvr_fake_gtt_pages);
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
 * native TTM/GTT allocator in this port. BOs use pinned pages and a video
 * MMU address table; VMA, file and DMA references retain their storage.
 * Video fences track real engine submissions. The union sizes match the
 * golden UAPI; this is separate from the still-stubbed Android SGX fences.
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

/* Legacy TTM placement flag (include/drm/ttm/ttm_placement.h): the caller
 * permits other DRM files to reference/lookup this object. */
#define SANTOS_TTM_PL_FLAG_SHARED (1u << 20)

#define SANTOS_FAKE_TTM_MAX 1024

struct santos_fake_ttm_bo {
	uint32_t handle;
	uint32_t refs;
	uint64_t size;
	uint32_t placement;
	uint64_t gpu_offset;
	uint64_t map_handle;
	void *cpu;
	uint8_t mmu_mapped;
	uint8_t cpu_dirty;	/* CPU wrote; flush before FW use */
	uint8_t shareable;	/* created with legacy TTM_PL_FLAG_SHARED */
	uint32_t synccpu;	/* aggregate TTM_REF_SYNCCPU_WRITE grabs */
	uint32_t submitting;	/* kernel reservation: reloc..engine submit */
	uint32_t last_seq;	/* last engine sequence using this BO */
	void (*unmap)(u64 gpu_offset, u64 size);
};

void santos_vdx_mmu_unmap_pages(u64 gpu_offset, u64 size);

static struct santos_fake_ttm_bo santos_fake_ttm_bos[SANTOS_FAKE_TTM_MAX];
static DEFINE_MUTEX(santos_fake_ttm_lock);
static DEFINE_MUTEX(santos_submit_lock);
/* CPU write grabs (3.4 cpu_writers / bo->event_queue analogue). A GPU submit
 * that validates a grabbed BO waits here until RELEASE; the generation counter
 * makes the check/sleep race-free. The same work queue carries submit-
 * reservation transitions (submitting flag), so a SYNCCPU GRAB cannot start
 * while the kernel is between relocation and engine submission. */
static DECLARE_WAIT_QUEUE_HEAD(santos_bo_synccpu_wq);
static atomic_t santos_bo_synccpu_gen = ATOMIC_INIT(0);
#define SANTOS_SYNCCPU_MODE_WRITE	(1 << 1)
#define SANTOS_SYNCCPU_MODE_NO_BLOCK	(1 << 2)
#define SANTOS_WAITIDLE_MODE_NO_BLOCK	(1 << 1)
static uint32_t santos_fake_ttm_next = 1;
static uint64_t santos_fake_ttm_gpu_next = 0x10000000;
static uint64_t santos_fake_ttm_map_next = 0x40000000;

/* GPU virtual address ranges returned to the allocator when a BO storage is
 * released. MMU unmap raised the engine invalidation flag, so a recycled range
 * is remapped before the next submit consumes the page tables. */
struct santos_gpu_va_range {
	struct list_head link;
	uint64_t start;
	uint64_t size;
};
static LIST_HEAD(santos_gpu_va_free);

struct santos_bo_file_ref {
	struct list_head link;
	struct file *filp;
	struct santos_fake_ttm_bo *bo;
	u32 count;
	u32 synccpu;	/* this file's outstanding SYNCCPU write grabs */
};
static LIST_HEAD(santos_bo_file_refs);
static void santos_vdx_bo_put_locked(struct santos_fake_ttm_bo *bo);
static void santos_vdx_drain_pending_locked(void);
static void santos_vdx_arm_pending_work_locked(void);
static void santos_vdx_ctxs_remove(struct file *filp);

static struct santos_bo_file_ref *
santos_bo_file_ref_locked(struct file *filp, u32 handle)
{
	struct santos_bo_file_ref *ref;

	list_for_each_entry(ref, &santos_bo_file_refs, link)
		if (ref->filp == filp && ref->bo->handle == handle)
			return ref;
	return NULL;
}

static int santos_bo_add_file_ref_locked(struct santos_fake_ttm_bo *bo,
					struct file *filp)
{
	struct santos_bo_file_ref *ref = santos_bo_file_ref_locked(filp, bo->handle);

	if (bo->refs == UINT_MAX)
		return -EOVERFLOW;
	if (!ref) {
		ref = kzalloc(sizeof(*ref), GFP_KERNEL);
		if (!ref)
			return -ENOMEM;
		ref->filp = filp;
		ref->bo = bo;
		list_add(&ref->link, &santos_bo_file_refs);
	}
	ref->count++;
	bo->refs++;
	return 0;
}

static struct santos_fake_ttm_bo *
santos_bo_owned_locked(struct file *filp, u32 handle)
{
	struct santos_bo_file_ref *ref = santos_bo_file_ref_locked(filp, handle);

	/* The usage handle dies with the last PL_UNREF even if a SYNCCPU ref
	 * keeps the BO itself alive (3.4 separate ref hashes). */
	return (ref && ref->count) ? ref->bo : NULL;
}

/* 3.4 ref objects: a SYNCCPU grab lives in its own per-file hash and is NOT
 * released by UNREF(USAGE). The first grab (0->1) takes one BO keepalive
 * reference owned by the SYNCCPU ref; nested grabs share it. This helper
 * releases all grabs of the ref and drops the keepalive. Caller holds the BO
 * lock. */
static void santos_bo_file_ref_release_grabs_locked(struct santos_bo_file_ref *ref)
{
	if (!ref->synccpu)
		return;
	if (ref->bo->synccpu >= ref->synccpu)
		ref->bo->synccpu -= ref->synccpu;
	else
		ref->bo->synccpu = 0;
	ref->synccpu = 0;
	if (!ref->bo->synccpu) {
		atomic_inc(&santos_bo_synccpu_gen);
		wake_up_all(&santos_bo_synccpu_wq);
	}
	santos_vdx_bo_put_locked(ref->bo);	/* SYNCCPU keepalive */
}

/* Drop every reference this file-ref owns: the SYNCCPU keepalive (released
 * inside release_grabs_locked when grabs are outstanding) plus exactly
 * ref->count usage references. Caller holds the BO lock. */
static void santos_bo_file_ref_close_locked(struct santos_bo_file_ref *ref)
{
	u32 i;

	if (ref->synccpu)
		santos_bo_file_ref_release_grabs_locked(ref);
	for (i = 0; i < ref->count; i++)
		santos_vdx_bo_put_locked(ref->bo);
	list_del(&ref->link);
	kfree(ref);
}

static void santos_bo_vma_open(struct vm_area_struct *vma)
{
	struct santos_fake_ttm_bo *bo = vma->vm_private_data;

	mutex_lock(&santos_fake_ttm_lock);
	bo->refs++;
	mutex_unlock(&santos_fake_ttm_lock);
}

static void santos_bo_vma_close(struct vm_area_struct *vma)
{
	mutex_lock(&santos_fake_ttm_lock);
	santos_vdx_bo_put_locked(vma->vm_private_data);
	mutex_unlock(&santos_fake_ttm_lock);
}

static const struct vm_operations_struct santos_bo_vm_ops = {
	.open = santos_bo_vma_open,
	.close = santos_bo_vma_close,
};

static int shell_quiet = 1;
module_param(shell_quiet, int, 0644);
MODULE_PARM_DESC(shell_quiet, "1 = suppress hot-path breadcrumbs");

/* Exact 3.4 WAITIDLE is reservation + ttm_bo_wait only (no cache flush).
 * Keep a switch for A/B; default = 3.4 semantics. CPU-read invalidation now
 * happens at BO mmap (userspace read mapping). */
static int waitidle_flush;
module_param(waitidle_flush, int, 0644);
MODULE_PARM_DESC(waitidle_flush, "1 = clflush whole BO at WAITIDLE (legacy)");

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

/* First-fit within previously freed ranges, then bump the top. Returns 0 when
 * the 32-bit firmware address space is exhausted. Caller holds the BO lock. */
static uint64_t santos_gpu_va_alloc_locked(uint64_t size)
{
	struct santos_gpu_va_range *r, *tmp;

	list_for_each_entry_safe(r, tmp, &santos_gpu_va_free, link) {
		uint64_t start;

		if (r->size < size)
			continue;
		start = r->start;
		r->start += size;
		r->size -= size;
		if (!r->size) {
			list_del(&r->link);
			kfree(r);
		}
		return start;
	}
	if (santos_fake_ttm_gpu_next + size > (1ULL << 32))
		return 0;
	{
		uint64_t start = santos_fake_ttm_gpu_next;

		santos_fake_ttm_gpu_next += size;
		return start;
	}
}

/* Return a range to the allocator and coalesce neighbours. Caller holds the BO
 * lock. A failed node allocation only forfeits reuse of this range. */
static void santos_gpu_va_free_locked(uint64_t start, uint64_t size)
{
	struct santos_gpu_va_range *r, *n;
	struct list_head *pos;

	if (!start || !size || start + size > (1ULL << 32))
		return;

	list_for_each(pos, &santos_gpu_va_free) {
		r = list_entry(pos, struct santos_gpu_va_range, link);
		if (r->start > start)
			break;
	}
	r = kmalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		pr_warn_once("santos-pvr: GPU VA free-list node alloc failed; range not reused\n");
		return;
	}
	r->start = start;
	r->size = size;
	list_add_tail(&r->link, pos);

	for (;;) {
		if (list_is_last(&r->link, &santos_gpu_va_free))
			break;
		n = list_next_entry(r, link);
		if (r->start + r->size != n->start)
			break;
		r->size += n->size;
		list_del(&n->link);
		kfree(n);
	}
	while (r->link.prev != &santos_gpu_va_free) {
		struct santos_gpu_va_range *p = list_prev_entry(r, link);

		if (p->start + p->size != r->start)
			break;
		p->size += r->size;
		list_del(&r->link);
		kfree(r);
		r = p;
	}
}

static int santos_vdx_bo_mmap(struct vm_area_struct *vma)
{
	unsigned long pgoff = vma->vm_pgoff;
	unsigned long len = vma->vm_end - vma->vm_start;
	struct santos_fake_ttm_bo *bo = NULL;
	unsigned long off;
	int i;

	mutex_lock(&santos_fake_ttm_lock);
	for (i = 0; i < SANTOS_FAKE_TTM_MAX; i++) {
		if (santos_fake_ttm_bos[i].handle &&
		    (santos_fake_ttm_bos[i].map_handle >> PAGE_SHIFT) == pgoff) {
			bo = &santos_fake_ttm_bos[i];
			break;
		}
	}
	if (!bo || !bo->cpu || len > bo->size ||
	    !santos_bo_owned_locked(vma->vm_file, bo->handle)) {
		mutex_unlock(&santos_fake_ttm_lock);
		return -ENOENT;
	}
	for (off = 0; off < len; off += PAGE_SIZE) {
		struct page *page = santos_vdx_cpu_page(bo->cpu + off);
		unsigned long pfn;

		if (!page) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -EFAULT;
		}
		pfn = page_to_pfn(page);
		if (remap_pfn_range(vma, vma->vm_start + off, pfn,
				    PAGE_SIZE, vma->vm_page_prot)) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -EAGAIN;
		}
	}
	/* Userspace map = potential CPU read: invalidate firmware-written
	 * data once at mapping time (clflush = writeback + invalidate). */
	clflush_cache_range(bo->cpu, bo->size);
	/* UNREF may remove the handle while this VMA still maps its pages. */
	bo->refs++;
	vma->vm_private_data = bo;
	vma->vm_ops = &santos_bo_vm_ops;
	if (!shell_quiet)
		pr_info("santos-pvr: BO mmap h=%u off=0x%lx len=0x%lx\n",
			bo->handle, pgoff << PAGE_SHIFT, len);
	mutex_unlock(&santos_fake_ttm_lock);
	return 0;
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
		uint64_t size;
		int ret;

		if (!a->req.size || a->req.size > (64ULL << 20)) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -EINVAL;
		}
		size = ALIGN(a->req.size, (u64)PAGE_SIZE);

		memset(bo, 0, sizeof(*bo));
		if (!santos_fake_ttm_next) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -ENOSPC;
		}
		bo->gpu_offset = santos_gpu_va_alloc_locked(size);
		if (!bo->gpu_offset) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -ENOSPC;
		}
		/* Firmware has per-page MMU mappings; high-order physical
		 * contiguity is unnecessary and fails after normal fragmentation. */
		bo->cpu = santos_vdx_cpu_alloc(size);
		if (bo->cpu)
			clflush_cache_range(bo->cpu, size);
		if (!bo->cpu) {
			santos_gpu_va_free_locked(bo->gpu_offset, size);
			memset(bo, 0, sizeof(*bo));
			mutex_unlock(&santos_fake_ttm_lock);
			return -ENOMEM;
		}
		bo->handle = santos_fake_ttm_next++;
		ret = santos_bo_add_file_ref_locked(bo, file->filp);
		if (ret) {
			santos_gpu_va_free_locked(bo->gpu_offset, size);
			kvfree(bo->cpu);
			memset(bo, 0, sizeof(*bo));
			mutex_unlock(&santos_fake_ttm_lock);
			return ret;
		}
		bo->size = size;
		bo->placement = a->req.placement;
		bo->shareable = !!(a->req.placement & SANTOS_TTM_PL_FLAG_SHARED);
		bo->cpu_dirty = 1;
		bo->map_handle = santos_fake_ttm_map_next;
		santos_fake_ttm_map_next += size;
		santos_fake_ttm_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_fake_ttm_lock);

	if (!bo) {
		pr_err_once("santos-pvr: fake TTM table full (%d BOs); raise SANTOS_FAKE_TTM_MAX\n",
			    SANTOS_FAKE_TTM_MAX);
		return -ENOMEM;
	}

	if (!shell_quiet)
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
	int ret = -EINVAL;

	(void)dev; (void)file;

	mutex_lock(&santos_fake_ttm_lock);
	bo = santos_fake_ttm_find_locked(a->req.handle);
	if (bo) {
		/* Legacy TTM shareable contract (golden ttm_base_object_lookup):
		 * a foreign file may reference the object only when it was
		 * created with TTM_PL_FLAG_SHARED; the caller's own ledger node
		 * (PL_CREATE or a previous PL_REFERENCE) always permits a
		 * nested reference. */
		if (!santos_bo_file_ref_locked(file->filp, a->req.handle) &&
		    !bo->shareable) {
			mutex_unlock(&santos_fake_ttm_lock);
			pr_err_ratelimited("santos-pvr: PL_REFERENCE non-shareable object h=%u\n",
					   a->req.handle);
			return -EINVAL;
		}
		ret = santos_bo_add_file_ref_locked(bo, file->filp);
		if (!ret)
			santos_fake_ttm_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_fake_ttm_lock);

	return ret;
}

static int santos_pvr_psb_pl_unref(struct drm_device *dev, void *arg,
				   struct drm_file *file)
{
	struct santos_ttm_pl_reference_req *a = arg;
	struct santos_bo_file_ref *ref;

	(void)dev; (void)file;

	mutex_lock(&santos_fake_ttm_lock);
	ref = santos_bo_file_ref_locked(file->filp, a->handle);
	if (!ref || !ref->count) {
		mutex_unlock(&santos_fake_ttm_lock);
		return -EINVAL;
	}
	ref->count--;
	/* Usage refs and the SYNCCPU keepalive are accounted separately: the
	 * last UNREF drops only the usage reference. */
	santos_vdx_bo_put_locked(ref->bo);
	if (!ref->count && !ref->synccpu) {
		list_del(&ref->link);
		kfree(ref);
	}
	mutex_unlock(&santos_fake_ttm_lock);

	return 0;
}

static void santos_vdx_bc(const char *fmt, ...);
static int santos_vdx_engine_wait_seq_call(uint32_t seq, unsigned int ms);
static uint32_t santos_vdx_engine_done_seq_call(void);

/* Patch 2 engine terminal/quiesce state (read-only engine ABI). Once the
 * engine leaves RUNNING the decode has failed for good; there is no automatic
 * recovery in this port. STOPPED_SAFE means the full golden MTX + core stop
 * proved that no MTX/MSVDX/RENDEC/MMU-DMAC agent can touch submitted backing
 * anymore (memory may be reclaimed, the decode is still failed); QUARANTINED
 * means the stop was not proven and all resources stay retained. */
int santos_vdx_engine_state(void);
#define SANTOS_VDX_STATE_RUNNING		0
#define SANTOS_VDX_STATE_QUIESCE_PENDING	1
#define SANTOS_VDX_STATE_QUIESCE_RUNNING	2
#define SANTOS_VDX_STATE_STOPPED_SAFE		3
#define SANTOS_VDX_STATE_QUARANTINED		4

static int santos_vdx_engine_state_call(void)
{
	int (*fn)(void) = symbol_get(santos_vdx_engine_state);
	int r;

	if (!fn)
		return -ENODEV;
	r = fn();
	symbol_put(santos_vdx_engine_state);
	return r;
}

/* Shell-side latch for the engine's one-way terminal failure. The engine
 * state is read-only and may disappear with the engine module; the latch keeps
 * DECODE_STATUS/WAITIDLE/SYNCCPU reporting the decode error after that. Shell
 * module lifetime is handled by the pending-set reference (see
 * santos_vdx_pending_ref_hold_locked): a QUARANTINED engine retains its
 * pending entries, so that reference naturally stays held until reboot and no
 * second permanent quarantine pin is needed. */
static atomic_t santos_vdx_terminal_latch = ATOMIC_INIT(0);
static uint32_t santos_decoding_err;

/* Record a terminal engine observation; the latch is acquired exactly once,
 * so repeated worker/WAITIDLE/DECODE_STATUS observations cannot increment
 * anything repeatedly. */
static void santos_vdx_note_engine_state(int st)
{
	if (st < SANTOS_VDX_STATE_QUIESCE_PENDING)
		return;
	if (atomic_xchg(&santos_vdx_terminal_latch, 1) == 0) {
		WRITE_ONCE(santos_decoding_err, 1);
		pr_err("santos-pvr: engine terminal failure observed (state=%d)\n",
		       st);
	}
}

/* True once the engine has entered the one-way terminal failure state. The
 * local latch dominates: a terminal failure observed earlier stays an error
 * even if the engine state symbol is gone. An engine that was never observed
 * terminal and is now unavailable (negative state) is not a decode error and
 * must not fabricate one. This must dominate any "nothing pending => success"
 * shortcut. */
static bool santos_vdx_engine_terminal(void)
{
	int st = santos_vdx_engine_state_call();

	if (st >= SANTOS_VDX_STATE_QUIESCE_PENDING) {
		santos_vdx_note_engine_state(st);
		return true;
	}
	return atomic_read(&santos_vdx_terminal_latch) != 0;
}

static int santos_pvr_psb_pl_synccpu(struct drm_device *dev, void *arg,
				     struct drm_file *file)
{
	struct santos_ttm_pl_synccpu_arg *a = arg;
	struct santos_bo_file_ref *ref;
	struct santos_fake_ttm_bo *bo;
	static int logged;
	u32 log_refs = 0, log_agg = 0;
	int ret = 0;

	(void)dev;

	if (a->op != 0 && a->op != 1)
		return -EINVAL;

	for (;;) {
		u32 seq;
		bool busy_seq;
		int gen;

		mutex_lock(&santos_fake_ttm_lock);
		ref = santos_bo_file_ref_locked(file->filp, a->handle);
		if (!ref) {
			mutex_unlock(&santos_fake_ttm_lock);
			pr_err_once("santos-pvr: SYNCCPU unknown handle=%u op=%u\n",
				    a->handle, a->op);
			return -EINVAL;
		}
		bo = ref->bo;
		if (a->op == 1) {	/* TTM_PL_SYNCCPU_OP_RELEASE */
			/* Only this file's grabs are released; another owner's
			 * SYNCCPU reservation must survive. */
			if (ref->synccpu) {
				ref->synccpu--;
				if (bo->synccpu)
					bo->synccpu--;
				if (!bo->synccpu) {
					atomic_inc(&santos_bo_synccpu_gen);
					wake_up_all(&santos_bo_synccpu_wq);
				}
				/* 0->1 took the single keepalive; 1->0 drops it. */
				if (!ref->synccpu)
					santos_vdx_bo_put_locked(bo);
			}
			if (!ref->count && !ref->synccpu) {
				log_refs = 0;
				log_agg = 0;
				list_del(&ref->link);
				kfree(ref);
			} else {
				log_refs = ref->synccpu;
				log_agg = bo->synccpu;
			}
			break;
		}
		/* A terminal decode must not hand the BO to CPU as if the
		 * hardware had finished with it. RELEASE (op==1) above stays
		 * functional so user reservations can still be dropped after a
		 * failure; only the GRAB path reports the decode error. */
		if (santos_vdx_engine_terminal()) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -EIO;
		}
		/* 3.4 ttm_bo_synccpu_write_grab(): every GRAB takes a write
		 * reservation. Wait for the last submission using this BO to
		 * complete and for any in-flight kernel reservation to finish,
		 * then hold off submissions until RELEASE. */
		seq = bo->last_seq;
		busy_seq = seq &&
			   (s32)(santos_vdx_engine_done_seq_call() - seq) < 0;
		if (!busy_seq && !bo->submitting) {
			if (ref->synccpu == UINT_MAX || bo->synccpu == UINT_MAX) {
				ret = -EOVERFLOW;
				break;
			}
			if (!ref->synccpu) {
				/* First grab of this file-ref owns the single
				 * BO keepalive; nested grabs share it. */
				if (bo->refs == UINT_MAX) {
					ret = -EOVERFLOW;
					break;
				}
				bo->refs++;
			}
			ref->synccpu++;
			bo->synccpu++;
			/* 3.4 write-grabs the BO for CPU access; the access
			 * mode is only a NO_BLOCK hint. Treat a write (or a
			 * bare) grab as CPU-dirty so the BO is flushed once
			 * before the next firmware use. */
			if ((a->access_mode & SANTOS_SYNCCPU_MODE_WRITE) ||
			    !a->access_mode)
				bo->cpu_dirty = 1;
			log_refs = ref->synccpu;
			log_agg = bo->synccpu;
			break;
		}
		if (a->access_mode & SANTOS_SYNCCPU_MODE_NO_BLOCK) {
			ret = -EBUSY;
			log_refs = ref->synccpu;
			log_agg = bo->synccpu;
			break;
		}
		gen = atomic_read(&santos_bo_synccpu_gen);
		mutex_unlock(&santos_fake_ttm_lock);
		/* The BO may be unref'd while we wait; loop re-looks it up. A
		 * concurrent submit can publish a newer last_seq, so also
		 * recheck the sequence instead of returning after one wait. */
		if (busy_seq) {
			ret = santos_vdx_engine_wait_seq_call(seq, 500);
			if (ret)
				return ret;
		} else {
			/* Reservation-only wait: wait_seq(0) would return
			 * immediately and spin in the loop. */
			if (wait_event_interruptible(santos_bo_synccpu_wq,
					atomic_read(&santos_bo_synccpu_gen) != gen))
				return -ERESTARTSYS;
		}
	}
	mutex_unlock(&santos_fake_ttm_lock);

	if (logged < 3) {
		logged++;
		santos_vdx_bc("synccpu op=%u handle=%u mode=0x%x file_refs=%u agg=%u",
			      a->op, a->handle, a->access_mode,
			      log_refs, log_agg);
	}
	return ret;
}



static void santos_vdx_drain_pending_locked(void);
static int santos_vdx_engine_wait_seq_call(uint32_t seq, unsigned int ms);
static uint32_t santos_vdx_engine_done_seq_call(void);

static int santos_pvr_psb_pl_waitidle(struct drm_device *dev, void *arg,
				      struct drm_file *file)
{
	struct santos_ttm_pl_waitidle_arg *a = arg;
	struct santos_fake_ttm_bo *bo;
	int ret = 0;

	(void)dev;

	for (;;) {
		u32 seq;
		int gen;
		long waited;

		mutex_lock(&santos_fake_ttm_lock);
		santos_vdx_drain_pending_locked();
		/* A terminal decode is an error even when the BO has no pending
		 * submission or its sequence looks reached: the abort drain must
		 * never be able to turn WAITIDLE into success. */
		if (santos_vdx_engine_terminal()) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -EIO;
		}
		bo = santos_bo_owned_locked(file->filp, a->handle);
		if (!bo) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -EINVAL;
		}
		/* 3.4 first reserves the BO, so an in-flight relocation/submit
		 * window cannot look idle just because last_seq is still old.
		 * Sample the shared generation under the BO lock; no pointer is
		 * kept across sleep, and publish/unreserve wakes this queue. */
		if (bo->submitting) {
			if (a->mode & SANTOS_WAITIDLE_MODE_NO_BLOCK) {
				mutex_unlock(&santos_fake_ttm_lock);
				return -EBUSY;
			}
			gen = atomic_read(&santos_bo_synccpu_gen);
			mutex_unlock(&santos_fake_ttm_lock);
			waited = wait_event_interruptible_timeout(santos_bo_synccpu_wq,
					atomic_read(&santos_bo_synccpu_gen) != gen,
					msecs_to_jiffies(500));
			if (santos_vdx_engine_terminal())
				return -EIO;
			if (waited <= 0)
				return waited ? -ERESTARTSYS : -ETIMEDOUT;
			continue;
		}
		seq = bo->last_seq;
		if (!seq ||
		    (s32)(santos_vdx_engine_done_seq_call() - seq) >= 0)
			break;
		if (a->mode & SANTOS_WAITIDLE_MODE_NO_BLOCK) {
			mutex_unlock(&santos_fake_ttm_lock);
			return -EBUSY;
		}
		mutex_unlock(&santos_fake_ttm_lock);
		/* 3.4 ttm_bo_wait: block until the submission using this BO
		 * has completed. A concurrent submit can publish a newer
		 * sequence while we wait, so re-lock and recheck instead of
		 * treating the first wait as final. A firmware error is not
		 * proof that DMA stopped; failed submissions stay pinned
		 * until a verified hardware quiesce/reboot. */
		ret = santos_vdx_engine_wait_seq_call(seq, 500);
		if (ret)
			return ret;
	}
	if (bo->cpu && waitidle_flush) {
		/* Legacy behavior: invalidate at WAITIDLE. Exact 3.4 WAITIDLE
		 * does no flush; CPU-read invalidation is done at BO mmap. */
		clflush_cache_range(bo->cpu, bo->size);
	}
	mutex_unlock(&santos_fake_ttm_lock);
	return 0;
}

static int santos_pvr_psb_pl_setstatus(struct drm_device *dev, void *arg,
				       struct drm_file *file)
{
	union santos_ttm_pl_setstatus_arg *a = arg;
	struct santos_fake_ttm_bo *bo;

	(void)dev; (void)file;

	mutex_lock(&santos_fake_ttm_lock);
	bo = santos_bo_owned_locked(file->filp, a->req.handle);
	if (bo) {
		bo->placement |= a->req.set_placement;
		bo->placement &= ~a->req.clr_placement;
		santos_fake_ttm_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_fake_ttm_lock);

	return bo ? 0 : -EINVAL;
}

/* The frozen TTM ABI uses handles, not sync_file descriptors. Keep the
 * engine module pinned and associate each handle with its real submission. */
int santos_vdx_engine_wait_seq(uint32_t seq, unsigned int timeout_ms);
struct santos_fence {
	struct list_head link;
	struct file *filp;
	u32 handle, seq;
	int (*wait)(u32, unsigned int);
};
static LIST_HEAD(santos_fences);
static u32 santos_fence_next = 1;

static struct santos_fence *santos_fence_find(struct file *filp, u32 handle)
{
	struct santos_fence *f;

	list_for_each_entry(f, &santos_fences, link)
		if (f->filp == filp && f->handle == handle)
			return f;
	return NULL;
}

static void santos_fence_free(struct santos_fence *f)
{
	if (f->wait)
		symbol_put(santos_vdx_engine_wait_seq);
	kfree(f);
}

static int santos_fence_query(union santos_ttm_fence_arg *a,
			      struct drm_file *file, bool finish)
{
	u32 handle = a->signaled.handle, type = a->signaled.fence_type;
	struct santos_fence *f;
	int ret;

	if (type & ~1U)
		return -EINVAL;
	mutex_lock(&santos_fake_ttm_lock);
	f = santos_fence_find(file->filp, handle);
	if (!f) {
		ret = -EINVAL;
		goto out;
	}
	/* Engine completion never takes the BO lock. Holding it here protects
	 * the handle against concurrent UNREF/close during the bounded wait. */
	ret = f->wait(f->seq, finish && type ? 500 : 0);
	a->rep.signaled_types = ret == 0 ? 1 : 0;
	a->rep.fence_error = ret == -EIO ? -EIO : 0;
	if ((!finish && ret == -ETIMEDOUT) || ret == -EIO)
		ret = 0;
out:
	mutex_unlock(&santos_fake_ttm_lock);
	return ret;
}

static int santos_pvr_psb_fence_signaled(struct drm_device *dev, void *arg,
					 struct drm_file *file)
{
	return santos_fence_query(arg, file, false);
}

static int santos_pvr_psb_fence_finish(struct drm_device *dev, void *arg,
				       struct drm_file *file)
{
	return santos_fence_query(arg, file, true);
}

static int santos_pvr_psb_fence_unref(struct drm_device *dev, void *arg,
				      struct drm_file *file)
{
	struct santos_ttm_fence_unref_arg *a = arg;
	struct santos_fence *f;
	int ret = -EINVAL;

	mutex_lock(&santos_fake_ttm_lock);
	f = santos_fence_find(file->filp, a->handle);
	if (f) {
		list_del(&f->link);
		santos_fence_free(f);
		ret = 0;
	}
	mutex_unlock(&santos_fake_ttm_lock);
	return ret;
}

/*
 * Gate 2: PSB_VIDEO_GETPARAM (golden 0x5e). Context lifecycle + the small
 * read-only queries the frozen VA userspace issues.
 */
struct santos_displaying_frame {
	uint32_t buf_handle;
	uint32_t width;
	uint32_t height;
	uint32_t size;
	uint32_t format;
	uint32_t luma_stride;
	uint32_t chroma_u_stride;
	uint32_t chroma_v_stride;
	uint32_t luma_offset;
	uint32_t chroma_u_offset;
	uint32_t chroma_v_offset;
	uint32_t reserved;
};

struct santos_video_getparam_arg {
	uint64_t key;
	uint64_t arg;
	uint64_t value;
};

#define SANTOS_LNC_VIDEO_DEVICE_INFO		0
#define SANTOS_LNC_VIDEO_GETPARAM_IMR_INFO	1
#define SANTOS_IMG_VIDEO_DECODE_STATUS		4
#define SANTOS_IMG_VIDEO_NEW_CONTEXT		5
#define SANTOS_IMG_VIDEO_RM_CONTEXT		6
#define SANTOS_IMG_VIDEO_UPDATE_CONTEXT		7
#define SANTOS_IMG_VIDEO_SET_DISPLAYING_FRAME	9
#define SANTOS_IMG_VIDEO_GET_DISPLAYING_FRAME	10
#define SANTOS_IMG_VIDEO_GET_HDMI_STATE		11
#define SANTOS_IMG_VIDEO_SET_HDMI_STATE		12
#define SANTOS_PNW_VIDEO_QUERY_ENTRY		13
#define SANTOS_IMG_VIDEO_IED_STATE		15

struct santos_vdx_ctx {
	struct list_head list;
	struct file *filp;
	uint64_t ctx_type;
};

static LIST_HEAD(santos_vdx_ctxs);
static DEFINE_MUTEX(santos_vdx_ctxs_lock);
static struct santos_displaying_frame santos_displaying;
static int santos_hdmi_state;

static void santos_vdx_ctxs_remove(struct file *filp)
{
	struct santos_vdx_ctx *ctx, *tmp;

	mutex_lock(&santos_vdx_ctxs_lock);
	list_for_each_entry_safe(ctx, tmp, &santos_vdx_ctxs, list) {
		if (ctx->filp == filp) {
			list_del(&ctx->list);
			kfree(ctx);
		}
	}
	mutex_unlock(&santos_vdx_ctxs_lock);
}

static void santos_vdx_file_close(struct file *filp)
{
	struct santos_bo_file_ref *ref, *tmp;
	struct santos_fence *f, *next;

	santos_vdx_ctxs_remove(filp);
	mutex_lock(&santos_fake_ttm_lock);
	santos_vdx_drain_pending_locked();
	santos_vdx_arm_pending_work_locked();
	list_for_each_entry_safe(f, next, &santos_fences, link) {
		if (f->filp == filp) {
			list_del(&f->link);
			santos_fence_free(f);
		}
	}
	list_for_each_entry_safe(ref, tmp, &santos_bo_file_refs, link) {
		if (ref->filp != filp)
			continue;
		/* File close drops this file's usage refs and releases its
		 * SYNCCPU grabs (3.4 ttm_object_file_release walks all ref
		 * objects); other owners' reservations must survive. */
		santos_bo_file_ref_close_locked(ref);
	}
	mutex_unlock(&santos_fake_ttm_lock);
}


/*
 * Gate 3b: CMDBUF validation + MMU mapping + relocation + snapshot.
 * No MTX submission yet (3c). Invariants:
 *  - page-table writes are flushed (psb_mmu_insert_pages + wmb) before the
 *    MMU invalidation flag is raised;
 *  - every validated BO is pinned (refs++) for the whole handler and
 *    released only after the snapshot is complete.
 */
struct santos_psb_validate_req {
	uint64_t set_flags;
	uint64_t clear_flags;
	uint64_t next;
	uint64_t presumed_gpu_offset;
	uint32_t buffer_handle;
	uint32_t presumed_flags;
	uint32_t pad64;
	uint32_t unfence_flag;
};

struct santos_psb_validate_rep {
	uint64_t gpu_offset;
	uint32_t placement;
	uint32_t fence_type_mask;
};

struct santos_psb_validate_arg {
	uint64_t handled;
	uint64_t ret;
	union {
		struct santos_psb_validate_req req;
		struct santos_psb_validate_rep rep;
	} d;
};

struct santos_drm_psb_cmdbuf_arg {
	uint64_t buffer_list;
	uint64_t fence_arg;
	uint32_t cmdbuf_handle;
	uint32_t cmdbuf_offset;
	uint32_t cmdbuf_size;
	uint32_t reloc_handle;
	uint32_t reloc_offset;
	uint32_t num_relocs;
	uint32_t fence_flags;
	uint32_t engine;
};

struct santos_drm_psb_reloc {
	uint32_t reloc_op;
	uint32_t where;
	uint32_t buffer;
	uint32_t mask;
	uint32_t shift;
	uint32_t pre_add;
	uint32_t background;
	uint32_t dst_buffer;
	uint32_t arg0;
	uint32_t arg1;
};

#define SANTOS_PSB_RELOC_OP_OFFSET	0
#define SANTOS_PSB_RELOC_SHIFT_MASK	0x0000FFFF
#define SANTOS_PSB_RELOC_ALSHIFT_MASK	0xFFFF0000
#define SANTOS_PSB_VALIDATE_MAX		64
#define SANTOS_PSB_ENGINE_DECODE	0

struct santos_vdx_val_entry {
	struct santos_fake_ttm_bo *bo;
};

static void santos_vdx_bo_put(struct santos_fake_ttm_bo *bo)
{
	mutex_lock(&santos_fake_ttm_lock);
	santos_vdx_bo_put_locked(bo);
	mutex_unlock(&santos_fake_ttm_lock);
}

static void santos_vdx_bo_put_locked(struct santos_fake_ttm_bo *bo)
{
	if (bo && bo->refs && --bo->refs == 0) {
		if (bo->synccpu) {
			bo->synccpu = 0;
			atomic_inc(&santos_bo_synccpu_gen);
			wake_up_all(&santos_bo_synccpu_wq);
		}
		if (bo->unmap) {
			bo->unmap(bo->gpu_offset, bo->size);
			symbol_put(santos_vdx_mmu_unmap_pages);
		}
		if (bo->cpu)
			kvfree(bo->cpu);
		santos_gpu_va_free_locked(bo->gpu_offset, bo->size);
		memset(bo, 0, sizeof(*bo));
	}
}

/* Queued commands reference BO gpu addresses; keep the pages pinned until
 * the matching engine sequence completes (3.4 TTM fence lifetime analogue). */
#define SANTOS_PENDING_MAX 64
struct santos_pending_pin {
	uint32_t seq;
	uint32_t nents;
	struct santos_fake_ttm_bo *bo[SANTOS_PSB_VALIDATE_MAX];
};
static struct santos_pending_pin santos_pending[SANTOS_PENDING_MAX];
static uint32_t santos_pending_head;
static uint32_t santos_pending_count;
static struct delayed_work santos_pending_work;
static void santos_vdx_drain_pending_locked(void);
static int santos_vdx_engine_wait_seq_call(uint32_t seq, unsigned int ms);
static uint32_t santos_vdx_engine_done_seq_call(void);

/* Last-close/last-ioctl cannot be the only drain point: completion happens in
 * the engine timer, so poll from a work item until every pin is released. */

/* The pending set is the shell-owned hardware lifetime: while it is non-empty
 * the shell module itself must stay resident. delayed_work is NOT a module
 * reference, so without this an unload could begin after the last fd closes
 * (file close retains the entries but takes no module ref) and discard the
 * quarantine bookkeeping before the delayed worker observes the final engine
 * state. One aggregate reference is taken on the pending 0 -> 1 transition
 * (publication, under the same BO lock) and released only on the 1 -> 0
 * transition after the normal completion drain or the verified STOPPED_SAFE
 * abort drain. QUIESCE_PENDING/QUIESCE_RUNNING and QUARANTINED keep entries,
 * so the reference stays held; file close and the poll stopping never release
 * it. Caller holds the BO lock. */
static void santos_vdx_pending_ref_hold_locked(void)
{
	if (santos_pending_count == 1)
		__module_get(THIS_MODULE);
}

static void santos_vdx_pending_ref_drop_locked(void)
{
	if (!santos_pending_count)
		module_put(THIS_MODULE);
}

/* Abort drain (BK-2): a terminal decode can never complete. Once the engine
 * published STOPPED_SAFE no MTX/MSVDX/RENDEC/MMU-DMAC agent can access the
 * submitted backing anymore, so every outstanding pending entry is released
 * exactly once, in FIFO order, through the same destruction primitive as the
 * normal drain (drops the BO pin, its MMU mapping/VA and the module ref when
 * the last owner goes away). eng_seq_done is never advanced and no completion
 * is signaled: an aborted job stays aborted. Caller holds the BO lock. */
static void santos_vdx_abort_pending_locked(void)
{
	while (santos_pending_count) {
		struct santos_pending_pin *pp =
			&santos_pending[santos_pending_head];
		u32 i;

		for (i = 0; i < pp->nents; i++)
			santos_vdx_bo_put_locked(pp->bo[i]);
		santos_pending_head =
			(santos_pending_head + 1) % SANTOS_PENDING_MAX;
		santos_pending_count--;
		if (!santos_pending_count)
			santos_vdx_pending_ref_drop_locked();
	}
}

/* Called with the BO lock held after pending publication or any engine state
 * change.
 *  - STOPPED_SAFE: take the safe abort-release route now. Publication and
 *    this check are serialized by the BO lock, which also closes the
 *    late-pending race (an entry published after the terminal transition is
 *    still drained here);
 *  - QUARANTINED (or the engine gone): retain every pin/map/ref and stop the
 *    pointless 20 ms poll; reboot is required;
 *  - otherwise (RUNNING or quiesce in progress): keep polling until the
 *    engine resolves to STOPPED_SAFE or QUARANTINED. */
static void santos_vdx_arm_pending_work_locked(void)
{
	int st;

	if (!santos_pending_count)
		return;
	st = santos_vdx_engine_state_call();
	santos_vdx_note_engine_state(st);
	if (st == SANTOS_VDX_STATE_STOPPED_SAFE) {
		santos_vdx_abort_pending_locked();
		return;
	}
	if (st == SANTOS_VDX_STATE_QUARANTINED || st < 0) {
		pr_err_ratelimited("santos-pvr: engine state %d; %u pending entries retained until reboot\n",
				   st, santos_pending_count);
		return;
	}
	schedule_delayed_work(&santos_pending_work,
			      msecs_to_jiffies(20));
}

static void santos_vdx_drain_pending_locked(void)
{
	u32 done = santos_vdx_engine_done_seq_call();

	while (santos_pending_count) {
		struct santos_pending_pin *pp =
			&santos_pending[santos_pending_head];
		u32 i;

		if ((s32)(done - pp->seq) < 0)
			break;
		for (i = 0; i < pp->nents; i++)
			santos_vdx_bo_put_locked(pp->bo[i]);
		santos_pending_head =
			(santos_pending_head + 1) % SANTOS_PENDING_MAX;
		santos_pending_count--;
		if (!santos_pending_count)
			santos_vdx_pending_ref_drop_locked();
	}
}

static void santos_pending_work_fn(struct work_struct *work)
{
	int st;

	(void)work;

	mutex_lock(&santos_fake_ttm_lock);
	st = santos_vdx_engine_state_call();
	santos_vdx_note_engine_state(st);
	if (st == SANTOS_VDX_STATE_STOPPED_SAFE) {
		/* Terminal failure with a proven safe stop: release all
		 * outstanding pins exactly once, never as completion. */
		santos_vdx_abort_pending_locked();
	} else if (st == SANTOS_VDX_STATE_QUARANTINED || st < 0) {
		/* Unsafe/unproven stop: retain everything; the arm helper
		 * reports the quarantine and stops the poll. */
	} else {
		/* RUNNING or quiesce in progress: normal done_seq drain first
		 * (completions that landed before the failure), then poll. */
		santos_vdx_drain_pending_locked();
	}
	santos_vdx_arm_pending_work_locked();
	mutex_unlock(&santos_fake_ttm_lock);
}

/* 3.4 ttm_eu_reserve_buffers analogue. The kernel takes a per-BO reservation
 * for the whole relocation -> engine-submit window:
 *  - entry blocks (interruptibly) while any listed BO has a CPU write grab or
 *    is already reserved;
 *  - once set, a SYNCCPU GRAB observes bo->submitting under the same BO lock
 *    and waits (or returns -EBUSY with NO_BLOCK) until the window closes.
 * The generation counter closes the check/sleep race on both sides. */
static int santos_vdx_reserve_bos(struct santos_fake_ttm_bo **bos, uint32_t n)
{
	for (;;) {
		bool busy = false;
		int gen;
		uint32_t i;

		mutex_lock(&santos_fake_ttm_lock);
		gen = atomic_read(&santos_bo_synccpu_gen);
		for (i = 0; i < n; i++)
			if (bos[i]->synccpu || bos[i]->submitting) {
				busy = true;
				break;
			}
		if (!busy) {
			for (i = 0; i < n; i++)
				bos[i]->submitting++;
			mutex_unlock(&santos_fake_ttm_lock);
			return 0;
		}
		mutex_unlock(&santos_fake_ttm_lock);
		if (wait_event_interruptible(santos_bo_synccpu_wq,
				atomic_read(&santos_bo_synccpu_gen) != gen))
			return -ERESTARTSYS;
	}
}

static void santos_vdx_unreserve_bos_locked(struct santos_fake_ttm_bo **bos,
					    uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		if (bos[i]->submitting)
			bos[i]->submitting--;
	atomic_inc(&santos_bo_synccpu_gen);
	wake_up_all(&santos_bo_synccpu_wq);
}

static void santos_vdx_unreserve_bos(struct santos_fake_ttm_bo **bos, uint32_t n)
{
	mutex_lock(&santos_fake_ttm_lock);
	santos_vdx_unreserve_bos_locked(bos, n);
	mutex_unlock(&santos_fake_ttm_lock);
}

/* Add a BO to the reservation set if it is not already listed. */
static void santos_vdx_add_resv(struct santos_fake_ttm_bo **bos, uint32_t *n,
				struct santos_fake_ttm_bo *bo)
{
	uint32_t i;

	if (!bo)
		return;
	for (i = 0; i < *n; i++)
		if (bos[i] == bo)
			return;
	bos[(*n)++] = bo;
}

static int santos_vdx_validate_list(struct santos_drm_psb_cmdbuf_arg *a,
				    struct santos_vdx_val_entry *ents,
				    uint32_t *nents, struct file *filp)
{
	uint64_t next = a->buffer_list;
	uint32_t n = 0;

	while (next && n < SANTOS_PSB_VALIDATE_MAX) {
		struct santos_psb_validate_arg va;
		u64 node_addr = next;
		struct santos_fake_ttm_bo *bo;

		if (copy_from_user(&va, (void __user *)(unsigned long)next,
				   sizeof(va)))
			return -EFAULT;

		mutex_lock(&santos_fake_ttm_lock);
		bo = santos_bo_owned_locked(filp, va.d.req.buffer_handle);
		if (bo) {
			bo->refs++;
			ents[n].bo = bo;
		}
		mutex_unlock(&santos_fake_ttm_lock);
		if (!bo)
			return -EINVAL;

		/* Publish each pin before a later copy/validation can fail. */
		*nents = ++n;
		next = va.d.req.next;
		va.d.rep.gpu_offset = bo->gpu_offset;
		va.d.rep.placement = bo->placement;
		va.d.rep.fence_type_mask = 1;
		va.handled = 1;
		va.ret = 0;
		if (copy_to_user((void __user *)(unsigned long)node_addr, &va,
				 sizeof(va)))
			return -EFAULT;

	}
	return next ? -E2BIG : 0;
}

static int santos_vdx_apply_relocs(struct santos_drm_psb_cmdbuf_arg *a,
				   struct santos_vdx_val_entry *ents,
				   uint32_t nents, struct file *filp)
{
	struct santos_fake_ttm_bo *reloc_bo;
	uint32_t i;

	if (!a->num_relocs)
		return 0;
	if (a->num_relocs > 4096)
		return -EINVAL;

	mutex_lock(&santos_fake_ttm_lock);
	reloc_bo = santos_bo_owned_locked(filp, a->reloc_handle);
	if (reloc_bo)
		reloc_bo->refs++;
	mutex_unlock(&santos_fake_ttm_lock);
	if (!reloc_bo)
		return -EINVAL;

	for (i = 0; i < a->num_relocs; i++) {
		struct santos_drm_psb_reloc r;
		uint64_t off = (u64)a->reloc_offset + i * sizeof(r);
		struct santos_fake_ttm_bo *src, *dst;
		uint32_t val, shift, align;
		u64 dst_off;

		if (off + sizeof(r) > reloc_bo->size) {
			santos_vdx_bo_put(reloc_bo);
			return -EINVAL;
		}
		memcpy(&r, reloc_bo->cpu + off, sizeof(r));

		if (r.reloc_op != SANTOS_PSB_RELOC_OP_OFFSET ||
		    r.buffer >= nents || r.dst_buffer >= nents) {
			santos_vdx_bo_put(reloc_bo);
			return -EINVAL;
		}
		src = ents[r.buffer].bo;
		dst = ents[r.dst_buffer].bo;

		if (r.pre_add > src->size) {
			santos_vdx_bo_put(reloc_bo);
			return -EINVAL;
		}
		val = (uint32_t)(src->gpu_offset + r.pre_add);
		shift = (r.shift & SANTOS_PSB_RELOC_SHIFT_MASK);
		align = (r.shift & SANTOS_PSB_RELOC_ALSHIFT_MASK) >> 16;
		if (align >= 32 || shift >= 32) {
			santos_vdx_bo_put(reloc_bo);
			return -EINVAL;
		}
		val = ((val >> align) << shift);
		val = (r.background & ~r.mask) | (val & r.mask);
		dst_off = (u64)r.where << 2;
		if ((uint64_t)dst_off + 4 > dst->size) {
			santos_vdx_bo_put(reloc_bo);
			return -EINVAL;
		}
		*(uint32_t *)((char *)dst->cpu + dst_off) = val;
		dst->cpu_dirty = 1;
	}
	santos_vdx_bo_put(reloc_bo);
	return 0;
}


/* MMU/engine calls resolve into santos-vdx.ko at call time. */
int santos_vdx_mmu_init(void);
int santos_vdx_mmu_map_pages(uint64_t gpu_offset, void *cpu, uint64_t size);
void santos_vdx_mmu_invalidate(void);
int santos_vdx_engine_init(void);
int santos_vdx_engine_submit(void *cmd, unsigned int size, uint32_t *seq);
int santos_vdx_engine_wait_seq(uint32_t seq, unsigned int timeout_ms);
int santos_vdx_engine_wait_idle(unsigned int timeout_ms);
uint32_t santos_vdx_engine_done_seq(void);

static int santos_vdx_engine_init_call(void)
{
	int (*fn)(void) = symbol_get(santos_vdx_engine_init);
	int r;

	if (!fn)
		return -ENODEV;
	r = fn();
	symbol_put(santos_vdx_engine_init);
	return r;
}

static int santos_vdx_engine_submit_call(void *cmd, unsigned int size,
					 uint32_t *seq)
{
	int (*fn)(void *, unsigned int, uint32_t *) =
		symbol_get(santos_vdx_engine_submit);
	int r;

	if (!fn)
		return -ENODEV;
	r = fn(cmd, size, seq);
	symbol_put(santos_vdx_engine_submit);
	return r;
}

static int santos_vdx_engine_wait_seq_call(uint32_t seq, unsigned int ms)
{
	int (*fn)(uint32_t, unsigned int) =
		symbol_get(santos_vdx_engine_wait_seq);
	int r;

	if (!fn)
		return -ENODEV;
	r = fn(seq, ms);
	symbol_put(santos_vdx_engine_wait_seq);
	return r;
}

static uint32_t santos_vdx_engine_done_seq_call(void)
{
	uint32_t (*fn)(void) = symbol_get(santos_vdx_engine_done_seq);
	uint32_t r;

	if (!fn)
		return 0;
	r = fn();
	symbol_put(santos_vdx_engine_done_seq);
	return r;
}

static int santos_vdx_mmu_init_call(void)
{
	int (*fn)(void) = symbol_get(santos_vdx_mmu_init);
	int r;

	if (!fn)
		return -ENODEV;
	r = fn();
	symbol_put(santos_vdx_mmu_init);
	return r;
}

static int santos_vdx_mmu_map_call(struct santos_fake_ttm_bo *bo)
{
	int (*fn)(uint64_t, void *, uint64_t) =
		symbol_get(santos_vdx_mmu_map_pages);
	void (*unmap)(u64, u64) = symbol_get(santos_vdx_mmu_unmap_pages);
	int r;

	if (!fn || !unmap) {
		if (fn)
			symbol_put(santos_vdx_mmu_map_pages);
		if (unmap)
			symbol_put(santos_vdx_mmu_unmap_pages);
		return -ENODEV;
	}
	r = fn(bo->gpu_offset, bo->cpu, bo->size);
	symbol_put(santos_vdx_mmu_map_pages);
	if (r)
		symbol_put(santos_vdx_mmu_unmap_pages);
	else
		/* Pins the engine/MMU module until this BO is truly unused. */
		bo->unmap = unmap;
	return r;
}

static void santos_vdx_mmu_invalidate_call(void)
{
	void (*fn)(void) = symbol_get(santos_vdx_mmu_invalidate);

	if (!fn)
		return;
	fn();
	symbol_put(santos_vdx_mmu_invalidate);
}


static atomic_t santos_vdx_bc_seq = ATOMIC_INIT(0);

static int sum_dump;
module_param(sum_dump, int, 0444);
MODULE_PARM_DESC(sum_dump, "1 = dump full BO sums and control words per CMDBUF");


static void santos_vdx_bc(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	if (shell_quiet)
		return;
	n = scnprintf(buf, sizeof(buf), "SANTOS_VDX_SHELL_BC[%d] ",
		      atomic_inc_return(&santos_vdx_bc_seq));
	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
	va_end(ap);
	printk(KERN_EMERG "%s\n", buf);
}

static int santos_pvr_psb_cmdbuf(struct drm_device *dev, void *arg,
				 struct drm_file *file)
{
	struct santos_fake_ttm_bo *cmdbuf_bo = NULL, *reloc_bo = NULL;
	struct santos_fake_ttm_bo *resv[SANTOS_PSB_VALIDATE_MAX + 2];
	struct santos_vdx_val_entry ents[SANTOS_PSB_VALIDATE_MAX];
	struct santos_drm_psb_cmdbuf_arg a;
	uint32_t nents = 0, nresv = 0, i, submitted_seq = 0;
	struct santos_fence *fence = NULL;
	bool reserved = false;
	void *snap = NULL;
	int ret = 0;

	(void)dev; (void)file;

	memcpy(&a, arg, sizeof(a));
	santos_vdx_bc("cmdbuf entry size=%u engine=%u", a.cmdbuf_size, a.engine);
	if (a.cmdbuf_size == 0 || a.cmdbuf_size > PAGE_SIZE)
		return -EINVAL;
	if (a.engine != SANTOS_PSB_ENGINE_DECODE) {
		pr_info("santos-pvr: VDX CMDBUF engine=%u not decode\n", a.engine);
		return -EINVAL;
	}
	/* BK-2 terminal gate: once the decode path is terminal, refuse new
	 * work before any new VDX MMU mapping/invalidation or relocation side
	 * effect. The engine may still go terminal after this gate; that race
	 * is covered by the Patch 2 submit contract and the late-publication
	 * ownership path below. */
	if (santos_vdx_engine_terminal()) {
		pr_err_ratelimited("santos-pvr: VDX CMDBUF refused (engine terminal)\n");
		return -EIO;
	}

	mutex_lock(&santos_submit_lock);
	ret = santos_vdx_validate_list(&a, ents, &nents, file->filp);
	santos_vdx_bc("validate done nents=%u ret=%d", nents, ret);
	if (ret) {
		pr_err("santos-pvr: VDX CMDBUF validate failed ret=%d\n", ret);
		goto out;
	}

	/* Pin the auxiliary BOs the ioctl path reads or writes before taking the
	 * reservation, so their storage cannot disappear from under us. */
	if (a.num_relocs) {
		mutex_lock(&santos_fake_ttm_lock);
		reloc_bo = santos_bo_owned_locked(file->filp, a.reloc_handle);
		if (reloc_bo)
			reloc_bo->refs++;
		mutex_unlock(&santos_fake_ttm_lock);
		if (!reloc_bo) {
			ret = -EINVAL;
			goto out;
		}
	}
	if (a.cmdbuf_handle) {
		mutex_lock(&santos_fake_ttm_lock);
		cmdbuf_bo = santos_bo_owned_locked(file->filp, a.cmdbuf_handle);
		if (cmdbuf_bo)
			cmdbuf_bo->refs++;
		mutex_unlock(&santos_fake_ttm_lock);
		if (!cmdbuf_bo ||
		    (uint64_t)a.cmdbuf_offset + a.cmdbuf_size > cmdbuf_bo->size) {
			ret = -EINVAL;
			goto out;
		}
	}

	/* Every BO the kernel touches for this ioctl joins the reservation:
	 * validated entries + relocation buffer + command buffer. */
	for (i = 0; i < nents; i++)
		santos_vdx_add_resv(resv, &nresv, ents[i].bo);
	santos_vdx_add_resv(resv, &nresv, reloc_bo);
	santos_vdx_add_resv(resv, &nresv, cmdbuf_bo);
	ret = santos_vdx_reserve_bos(resv, nresv);
	if (ret) {
		pr_err("santos-pvr: VDX CMDBUF reservation failed ret=%d\n", ret);
		goto out;
	}
	reserved = true;
	santos_vdx_bc("cmdbuf reserved nresv=%u", nresv);

	if (santos_vdx_mmu_init_call()) {
		ret = -ENODEV;
		goto out;
	}
	for (i = 0; i < nents; i++) {
		struct santos_fake_ttm_bo *bo = ents[i].bo;

		if (bo->mmu_mapped)
			continue;
		ret = santos_vdx_mmu_map_call(bo);
		if (ret) {
			pr_err("santos-pvr: VDX MMU map gpu=0x%llx ret=%d\n",
			       (unsigned long long)bo->gpu_offset, ret);
			goto out;
		}
		bo->mmu_mapped = 1;
	}

	wmb();
	santos_vdx_mmu_invalidate_call();
	santos_vdx_bc("mmu maps done, invalidate set");

	ret = santos_vdx_apply_relocs(&a, ents, nents, file->filp);
	santos_vdx_bc("relocs done ret=%d", ret);
	if (ret) {
		pr_err("santos-pvr: VDX relocs failed ret=%d\n", ret);
		goto out;
	}

	if (a.cmdbuf_handle) {
		snap = kmalloc(a.cmdbuf_size, GFP_KERNEL);
		if (!snap) {
			ret = -ENOMEM;
			goto out;
		}
		memcpy(snap, (char *)cmdbuf_bo->cpu + a.cmdbuf_offset,
		       a.cmdbuf_size);
		if (!shell_quiet)
			pr_info("santos-pvr: VDX CMDBUF snapshot size=%u v0=%08x v1=%08x v2=%08x v3=%08x\n",
			a.cmdbuf_size,
			a.cmdbuf_size >= 4 ? *(uint32_t *)snap : 0,
			a.cmdbuf_size >= 8 ? *((uint32_t *)snap + 1) : 0,
			a.cmdbuf_size >= 12 ? *((uint32_t *)snap + 2) : 0,
			a.cmdbuf_size >= 16 ? *((uint32_t *)snap + 3) : 0);
	}

	if (!snap) {
		ret = -EINVAL;
		goto out;
	}
	if (a.fence_arg && !(a.fence_flags & 1)) {
		fence = kzalloc(sizeof(*fence), GFP_KERNEL);
		if (!fence) {
			ret = -ENOMEM;
			goto out;
		}
		fence->wait = symbol_get(santos_vdx_engine_wait_seq);
		if (!fence->wait || !santos_fence_next) {
			ret = fence->wait ? -ENOSPC : -ENODEV;
			goto out;
		}
		fence->handle = santos_fence_next++;
		fence->filp = file->filp;
	}
	if (snap) {
		if (sum_dump && a.cmdbuf_size >= 20 &&
		    (((u32 *)snap)[0] & 0xff00) == 0x8100) {
			u32 crtl = ((u32 *)snap)[2];
			u32 cw[16];
			u32 j, k;
			struct santos_fake_ttm_bo *cbo = NULL;

			mutex_lock(&santos_fake_ttm_lock);
			for (k = 0; k < nents; k++)
				if ((u32)ents[k].bo->gpu_offset == crtl)
					cbo = ents[k].bo;
			if (cbo && cbo->cpu && cbo->size >= 64) {
				memcpy(cw, cbo->cpu, sizeof(cw));
				santos_vdx_bc("ctrl va=0x%x h=%u w0..7=%08x %08x %08x %08x %08x %08x %08x %08x",
					crtl, cbo->handle, cw[0], cw[1], cw[2], cw[3],
					cw[4], cw[5], cw[6], cw[7]);
				for (j = 0; j < 16; j++) {
					for (k = 0; k < nents; k++) {
						if (cw[j] == (u32)ents[k].bo->gpu_offset) {
							santos_vdx_bc("ctrl word[%u]=0x%x == bo[%u] gpu h=%u size=0x%llx",
								j, cw[j], k, ents[k].bo->handle,
								(unsigned long long)ents[k].bo->size);
						}
					}
				}
			} else {
				santos_vdx_bc("ctrl va=0x%x has NO matching validated BO", crtl);
			}
			mutex_unlock(&santos_fake_ttm_lock);
		}
		for (i = 0; i < nents; i++) {
			if (ents[i].bo->cpu && ents[i].bo->cpu_dirty) {
				clflush_cache_range(ents[i].bo->cpu,
						    ents[i].bo->size);
				ents[i].bo->cpu_dirty = 0;
			}
		}
		if (cmdbuf_bo && cmdbuf_bo->cpu)
			clflush_cache_range(cmdbuf_bo->cpu, cmdbuf_bo->size);
		santos_vdx_bc("engine init call");
		ret = santos_vdx_engine_init_call();
		if (ret) {
			pr_err("santos-pvr: VDX engine init failed ret=%d\n", ret);
			goto out;
		}
		{
			uint32_t seq = 0;

			/* Pin capacity + publish the sequence while the BO
			 * reservation is still held, then release it under the
			 * same lock that made last_seq visible: a GRAB can only
			 * become active after it sees the submitted sequence.
			 * The engine has no callbacks into the BO table. */
			mutex_lock(&santos_fake_ttm_lock);
			santos_vdx_drain_pending_locked();
			if (santos_pending_count == SANTOS_PENDING_MAX) {
				mutex_unlock(&santos_fake_ttm_lock);
				ret = -EBUSY;
				goto out;
			}
			santos_vdx_bc("engine submit call size=%u", a.cmdbuf_size);
			ret = santos_vdx_engine_submit_call(snap, a.cmdbuf_size,
							    &seq);
			santos_vdx_bc("engine submit ret=%d seq=%u", ret, seq);
			if (!ret) {
				struct santos_pending_pin *pp =
				    &santos_pending[(santos_pending_head +
						     santos_pending_count) %
						    SANTOS_PENDING_MAX];

				submitted_seq = seq;
				pp->seq = seq;
				pp->nents = nents;
				for (i = 0; i < nents; i++) {
					ents[i].bo->refs++;
					ents[i].bo->last_seq = seq;
					pp->bo[i] = ents[i].bo;
				}
				santos_pending_count++;
				santos_vdx_pending_ref_hold_locked();
				santos_vdx_arm_pending_work_locked();
			}
			santos_vdx_unreserve_bos_locked(resv, nresv);
			reserved = false;
			mutex_unlock(&santos_fake_ttm_lock);
		}
		/* CPU-visible invalidation for FW-written buffers happens at
		 * the UMD's WAITIDLE (per-BO). No blanket post-flush. */
		for (i = 0; sum_dump && i < nents; i++) {
			struct santos_fake_ttm_bo *b = ents[i].bo;
			u64 j;
			u32 sum = 0;
			u32 nz = 0;
			u32 firstnz = 0xffffffff;

			if (!b->cpu)
				continue;
			for (j = 0; j < b->size; j++) {
				u8 v = ((u8 *)b->cpu)[j];
				sum += v;
				if (v) {
					if (firstnz == 0xffffffff)
						firstnz = (u32)j;
					nz++;
				}
			}
			santos_vdx_bc("bo[%u] h=%u gpu=0x%llx size=0x%llx sum=0x%08x nz=%u firstnz=%u",
				i, b->handle, (unsigned long long)b->gpu_offset,
				(unsigned long long)b->size, sum, nz, firstnz);
		}
		if (ret) {
			pr_err("santos-pvr: VDX engine submit failed ret=%d\n", ret);
			goto out;
		}
		if (a.fence_arg && !(a.fence_flags & 1)) {
			struct {
				uint32_t handle;
				uint32_t fence_class;
				uint32_t fence_type;
				uint32_t signaled_types;
				uint32_t error;
			} rep;

			memset(&rep, 0, sizeof(rep));
			rep.handle = fence->handle;
			rep.fence_class = 0;
			rep.fence_type = 1;
			rep.signaled_types = fence->wait(submitted_seq, 0) == 0 ? 1 : 0;
			rep.error = 0;
			if (copy_to_user((void __user *)(unsigned long)a.fence_arg,
					 &rep, sizeof(rep)))
				ret = -EFAULT;
			if (!ret) {
				fence->seq = submitted_seq;
				mutex_lock(&santos_fake_ttm_lock);
				list_add(&fence->link, &santos_fences);
				mutex_unlock(&santos_fake_ttm_lock);
				fence = NULL;
			}
		}
		if (!shell_quiet)
			pr_info("santos-pvr: VDX CMDBUF submitted size=%u nents=%u ret=%d\n",
				a.cmdbuf_size, nents, ret);
	}

out:
	if (reserved)
		santos_vdx_unreserve_bos(resv, nresv);
	if (fence)
		santos_fence_free(fence);
	kfree(snap);
	if (cmdbuf_bo)
		santos_vdx_bo_put(cmdbuf_bo);
	if (reloc_bo)
		santos_vdx_bo_put(reloc_bo);
	for (i = 0; i < nents; i++)
		santos_vdx_bo_put(ents[i].bo);
	mutex_unlock(&santos_submit_lock);
	return ret;
}

#define SANTOS_PSB_CMDBUF_ARG_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + SANTOS_PSB_CMDBUF_OFFSET, \
		struct santos_drm_psb_cmdbuf_arg)

static int santos_pvr_psb_getparam(struct drm_device *dev, void *arg,
				   struct drm_file *file)
{
	struct santos_video_getparam_arg *a = arg;
	void __user *uval = (void __user *)(unsigned long)a->value;
	void __user *uarg = (void __user *)(unsigned long)a->arg;
	struct santos_vdx_ctx *ctx;
	uint32_t tmp32 = 0;
	uint64_t ctx_type = 0;
	int ret = 0;

	(void)dev;

	switch (a->key) {
	case SANTOS_LNC_VIDEO_DEVICE_INFO:
		tmp32 = ((uint32_t)SANTOS_PVR_PCI_DID << 16);
		if (copy_to_user(uval, &tmp32, sizeof(tmp32)))
			ret = -EFAULT;
		break;
	case SANTOS_LNC_VIDEO_GETPARAM_IMR_INFO: {
		uint32_t imr[2] = { 0, 0 };
		if (copy_to_user(uval, imr, sizeof(imr)))
			ret = -EFAULT;
		break;
	}
	case SANTOS_IMG_VIDEO_NEW_CONTEXT:
		if (copy_from_user(&ctx_type, uval, sizeof(ctx_type))) {
			ret = -EFAULT;
			break;
		}
		ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
		if (!ctx) {
			ret = -ENOMEM;
			break;
		}
		INIT_LIST_HEAD(&ctx->list);
		ctx->filp = file->filp;
		ctx->ctx_type = ctx_type;
		mutex_lock(&santos_vdx_ctxs_lock);
		list_add(&ctx->list, &santos_vdx_ctxs);
		mutex_unlock(&santos_vdx_ctxs_lock);
		pr_info("santos-pvr: VDX NEW_CONTEXT type=0x%llx\n",
			(unsigned long long)ctx_type);
		break;
	case SANTOS_IMG_VIDEO_RM_CONTEXT:
		santos_vdx_ctxs_remove(file->filp);
		break;
	case SANTOS_IMG_VIDEO_UPDATE_CONTEXT:
		if (copy_from_user(&ctx_type, uval, sizeof(ctx_type))) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&santos_vdx_ctxs_lock);
		list_for_each_entry(ctx, &santos_vdx_ctxs, list) {
			if (ctx->filp == file->filp) {
				ctx->ctx_type = ctx_type;
				break;
			}
		}
		mutex_unlock(&santos_vdx_ctxs_lock);
		break;
	case SANTOS_IMG_VIDEO_DECODE_STATUS: {
		uint32_t decode_err = READ_ONCE(santos_decoding_err);

		/* A Patch 2 terminal failure is a persistent decode error and
		 * must dominate every later state: STOPPED_SAFE only means the
		 * hardware stop was proven (memory is reclaimable), not that
		 * the decode succeeded; QUARANTINED remains an error too. */
		if (santos_vdx_engine_terminal())
			decode_err = 1;
		if (copy_to_user(uval, &decode_err, sizeof(decode_err)))
			ret = -EFAULT;
		break;
	}
	case SANTOS_IMG_VIDEO_SET_DISPLAYING_FRAME:
		if (copy_from_user(&santos_displaying, uval,
				   sizeof(santos_displaying)))
			ret = -EFAULT;
		break;
	case SANTOS_IMG_VIDEO_GET_DISPLAYING_FRAME:
		if (copy_to_user(uval, &santos_displaying,
				 sizeof(santos_displaying)))
			ret = -EFAULT;
		break;
	case SANTOS_IMG_VIDEO_GET_HDMI_STATE:
		if (copy_to_user(uval, &santos_hdmi_state,
				 sizeof(santos_hdmi_state)))
			ret = -EFAULT;
		break;
	case SANTOS_IMG_VIDEO_SET_HDMI_STATE:
		if (copy_from_user(&santos_hdmi_state, uval,
				   sizeof(santos_hdmi_state)))
			ret = -EFAULT;
		break;
	case SANTOS_PNW_VIDEO_QUERY_ENTRY: {
		uint32_t entry, count = 0;
		struct santos_vdx_ctx *c;

		if (copy_from_user(&entry, uarg, sizeof(entry))) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&santos_vdx_ctxs_lock);
		list_for_each_entry(c, &santos_vdx_ctxs, list)
			if ((c->ctx_type & 0xff) == entry)
				count++;
		mutex_unlock(&santos_vdx_ctxs_lock);
		if (copy_to_user(uval, &count, sizeof(count)))
			ret = -EFAULT;
		break;
	}
	case SANTOS_IMG_VIDEO_IED_STATE:
		tmp32 = 0;
		if (copy_to_user(uval, &tmp32, sizeof(tmp32)))
			ret = -EFAULT;
		break;
	default:
		pr_info("santos-pvr: VDX getparam unknown key %llu\n",
			(unsigned long long)a->key);
		ret = -EFAULT;
		break;
	}
	return ret;
}

#define SANTOS_PSB_CMDBUF_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + SANTOS_PSB_CMDBUF_OFFSET, struct santos_drm_psb_cmdbuf_arg)
#define SANTOS_PSB_GETPARAM_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + SANTOS_PSB_GETPARAM, \
		 struct santos_video_getparam_arg)

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
	[0x32] = { .cmd = SANTOS_PSB_VSYNC_SET_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_vsync_set,
	  .name = "PSB_VSYNC_SET" },
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
	[0x50] = { .cmd = SANTOS_PSB_CMDBUF_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_cmdbuf,
	  .name = "PSB_CMDBUF" },
	[0x5e] = { .cmd = SANTOS_PSB_GETPARAM_IOCTL,
	  .flags = 0,
	  .func = santos_pvr_psb_getparam,
	  .name = "PSB_VIDEO_GETPARAM" },
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
static_assert(ARRAY_SIZE(santos_pvr_ioctls) == 0x5f);

static struct pci_dev *santos_pvr_pdev;
static struct drm_device *santos_pvr_drm;

/* Golden psb_irq.c source bits. Default remains SGX only; an explicitly
 * registered VDX engine may share the existing vector without touching display. */
#define SANTOS_INT_ENABLE 0x20a0
#define SANTOS_INT_IDENTITY 0x20a4
#define SANTOS_INT_MASK 0x20a8
#define SANTOS_INT_SGX BIT(18)
#define SANTOS_INT_MSVDX BIT(19)

static void santos_irq_mask(void __iomem *regs)
{
	writel(readl(regs + SANTOS_INT_ENABLE) & ~SANTOS_INT_SGX,
	       regs + SANTOS_INT_ENABLE);
	writel(readl(regs + SANTOS_INT_MASK) | SANTOS_INT_SGX,
	       regs + SANTOS_INT_MASK);
	readl(regs + SANTOS_INT_MASK);
}

static irqreturn_t santos_sgx_interrupt(int irq, void *data)
{
	void __iomem *regs = santos_irq_regs;
	void (*vdx_service)(void) = READ_ONCE(santos_vdx_irq_service);
	u32 pending = readl(regs + SANTOS_INT_IDENTITY);
	u32 handled = 0;

	if (pending & SANTOS_INT_SGX) {
		/* Clear the SGX event and schedule MISR before acknowledging VDC. */
		santos_irq_service(data);
		handled |= SANTOS_INT_SGX;
	}
	if ((pending & SANTOS_INT_MSVDX) && vdx_service) {
		vdx_service();
		handled |= SANTOS_INT_MSVDX;
	}
	if (!handled)
		return IRQ_NONE;
	/* Acknowledge only sources we actually owned/serviced. */
	writel(handled, regs + SANTOS_INT_IDENTITY);
	readl(regs + SANTOS_INT_IDENTITY);
	return IRQ_HANDLED;
}

static void santos_vdx_irq_mask_locked(void)
{
	writel(readl(santos_irq_regs + SANTOS_INT_ENABLE) & ~SANTOS_INT_MSVDX,
	       santos_irq_regs + SANTOS_INT_ENABLE);
	writel(readl(santos_irq_regs + SANTOS_INT_MASK) | SANTOS_INT_MSVDX,
	       santos_irq_regs + SANTOS_INT_MASK);
	readl(santos_irq_regs + SANTOS_INT_MASK);
}

/* Caller holds santos_irq_lock. Unpublish before synchronize_irq: an SGX
 * interrupt can still arrive while the VDX source itself is masked. */
static void santos_vdx_irq_detach_locked(void)
{
	void (*stopped)(void);

	if (!santos_vdx_irq_service)
		return;
	stopped = santos_vdx_irq_stopped;
	santos_vdx_irq_mask_locked();
	WRITE_ONCE(santos_vdx_irq_service, NULL);
	synchronize_irq(santos_irq);
	santos_vdx_irq_stopped = NULL;
	/* Notify only after all service calls have finished. The provider pin
	 * and the engine's synchronizing fini still protect both lifetimes. */
	if (stopped)
		stopped();
	writel(SANTOS_INT_MSVDX, santos_irq_regs + SANTOS_INT_IDENTITY);
	writel((readl(santos_irq_regs + SANTOS_INT_MASK) & ~SANTOS_INT_MSVDX) |
	       santos_vdx_irq_saved_mask, santos_irq_regs + SANTOS_INT_MASK);
	writel((readl(santos_irq_regs + SANTOS_INT_ENABLE) & ~SANTOS_INT_MSVDX) |
	       santos_vdx_irq_saved_enable, santos_irq_regs + SANTOS_INT_ENABLE);
	readl(santos_irq_regs + SANTOS_INT_ENABLE);
	if (santos_vdx_irq_provider_pin) {
		santos_vdx_irq_provider_pin = NULL;
		symbol_put(PVRSRVDrmOpen);
	}
}

int santos_pvr_vdx_irq_register(void (*service)(void), void (*stopped)(void))
{
	int ret = 0;

	mutex_lock(&santos_irq_lock);
	if (!service || !stopped) {
		ret = -EINVAL;
		goto out;
	}
	if (santos_irq < 0 || !santos_irq_regs || santos_irq_stopping) {
		ret = -ENODEV;
		goto out;
	}
	if (santos_vdx_irq_service) {
		ret = -EBUSY;
		goto out;
	}
	/* Services owns the shared VDC BAR. Pin its module for the whole
	 * registration, not merely while fetching its exported symbol. */
	santos_vdx_irq_provider_pin = symbol_get(PVRSRVDrmOpen);
	if (!santos_vdx_irq_provider_pin) {
		ret = -ENODEV;
		goto out;
	}
	santos_vdx_irq_saved_enable = readl(santos_irq_regs + SANTOS_INT_ENABLE) &
				      SANTOS_INT_MSVDX;
	santos_vdx_irq_saved_mask = readl(santos_irq_regs + SANTOS_INT_MASK) &
				    SANTOS_INT_MSVDX;
	santos_vdx_irq_mask_locked();
	santos_vdx_irq_stopped = stopped;
	WRITE_ONCE(santos_vdx_irq_service, service);
	/* Callback/engine state must be visible before this source is enabled. */
	wmb();
	writel(readl(santos_irq_regs + SANTOS_INT_MASK) & ~SANTOS_INT_MSVDX,
	       santos_irq_regs + SANTOS_INT_MASK);
	writel(readl(santos_irq_regs + SANTOS_INT_ENABLE) | SANTOS_INT_MSVDX,
	       santos_irq_regs + SANTOS_INT_ENABLE);
	readl(santos_irq_regs + SANTOS_INT_ENABLE);
out:
	mutex_unlock(&santos_irq_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(santos_pvr_vdx_irq_register);

void santos_pvr_vdx_irq_unregister(void (*service)(void))
{
	mutex_lock(&santos_irq_lock);
	if (santos_vdx_irq_service == service && service)
		santos_vdx_irq_detach_locked();
	else
		WARN_ON_ONCE(santos_vdx_irq_service != NULL);
	mutex_unlock(&santos_irq_lock);
}
EXPORT_SYMBOL_GPL(santos_pvr_vdx_irq_unregister);

int santos_pvr_irq_start(void __iomem *irq_regs,
			 void __iomem *display_regs,
			 int (*service)(struct drm_device *))
{
	int ret;
	struct pci_dev *pdev = santos_pvr_pdev;
	u32 old_enable, old_mask;

	mutex_lock(&santos_irq_lock);
	if (!pdev || !irq_regs || !display_regs || !service) {
		ret = -ENODEV;
		goto out;
	}
	if (santos_irq >= 0 || pdev->msi_enabled || pdev->msix_enabled) {
		ret = -EBUSY;
		goto out;
	}
	WRITE_ONCE(santos_irq_stopping, false);
	mutex_lock(&santos_vsync_state_lock);
	santos_vsync_enabled = false;
	santos_vsync_epoch++;
	mutex_unlock(&santos_vsync_state_lock);
	old_enable = readl(irq_regs + SANTOS_INT_ENABLE);
	old_mask = readl(irq_regs + SANTOS_INT_MASK);
	santos_irq_mask(irq_regs);
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0)
		goto restore_source;
	ret = pci_irq_vector(pdev, 0);
	if (ret <= 0) {
		ret = ret ? ret : -EINVAL;
		goto free_vectors;
	}
	santos_irq = ret;
	santos_irq_regs = irq_regs;
	santos_vsync_regs = display_regs;
	santos_irq_service = service;
	ret = request_irq(santos_irq, santos_sgx_interrupt, 0,
			  "santos-sgx", santos_pvr_drm);
	if (ret)
		goto clear_state;
	/* Drain any pre-install event with the same real completion path. */
	service(santos_pvr_drm);
	writel(SANTOS_INT_SGX, irq_regs + SANTOS_INT_IDENTITY);
	readl(irq_regs + SANTOS_INT_IDENTITY);
	writel(readl(irq_regs + SANTOS_INT_MASK) & ~SANTOS_INT_SGX,
	       irq_regs + SANTOS_INT_MASK);
	writel(readl(irq_regs + SANTOS_INT_ENABLE) | SANTOS_INT_SGX,
	       irq_regs + SANTOS_INT_ENABLE);
	readl(irq_regs + SANTOS_INT_ENABLE);
	pr_info("santos-pvr: SGX MSI irq=%d enabled (Services MISR ready)\n",
		santos_irq);
	ret = 0;
	goto out;
clear_state:
	santos_irq = -1;
	santos_irq_regs = NULL;
	santos_vsync_regs = NULL;
	santos_irq_service = NULL;
free_vectors:
	pci_free_irq_vectors(pdev);
restore_source:
	writel((readl(irq_regs + SANTOS_INT_MASK) & ~SANTOS_INT_SGX) |
	       (old_mask & SANTOS_INT_SGX), irq_regs + SANTOS_INT_MASK);
	writel((readl(irq_regs + SANTOS_INT_ENABLE) & ~SANTOS_INT_SGX) |
	       (old_enable & SANTOS_INT_SGX), irq_regs + SANTOS_INT_ENABLE);
	readl(irq_regs + SANTOS_INT_ENABLE);
out:
	mutex_unlock(&santos_irq_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(santos_pvr_irq_start);

void santos_pvr_irq_stop(void)
{
	mutex_lock(&santos_irq_lock);
	WRITE_ONCE(santos_irq_stopping, true);
	mutex_lock(&santos_vsync_state_lock);
	santos_vsync_enabled = false;
	santos_vsync_epoch++;
	mutex_unlock(&santos_vsync_state_lock);
	wake_up_interruptible(&santos_vsync_waitq);
	/* A WAIT holds a source reference across its bounded poll. Keep the
	 * Services-owned BAR mapping alive until every such reader has left. */
	wait_event(santos_vsync_users_wq,
		   atomic_read(&santos_vsync_users) == 0);
	if (santos_irq >= 0) {
		/* Normal provider unload is barred by the registration pin. If
		 * an explicit provider teardown occurs, detach/synchronize its
		 * shared callback before the BAR/vector is destroyed. */
		santos_vdx_irq_detach_locked();
		santos_irq_mask(santos_irq_regs);
		/* free_irq waits for callbacks before provider/MISR teardown. */
		free_irq(santos_irq, santos_pvr_drm);
		writel(SANTOS_INT_SGX, santos_irq_regs + SANTOS_INT_IDENTITY);
		readl(santos_irq_regs + SANTOS_INT_IDENTITY);
		pci_free_irq_vectors(santos_pvr_pdev);
		santos_irq = -1;
		santos_irq_regs = NULL;
		santos_vsync_regs = NULL;
		santos_irq_service = NULL;
	} else {
		santos_irq_regs = NULL;
		santos_vsync_regs = NULL;
		santos_irq_service = NULL;
	}
	mutex_unlock(&santos_irq_lock);
}
EXPORT_SYMBOL_GPL(santos_pvr_irq_stop);

static u32 santos_pvr_vsync_read_frame_counter(void __iomem *regs)
{
	u32 high1, high2, low;

	/* The high and low fields are not synchronized; this is the exact
	 * golden high/low/high stability protocol used by Candidate3B. */
	do {
		high1 = (readl(regs + SANTOS_VDC_PIPEAFRAMEHIGH) &
			 SANTOS_PIPE_FRAME_HIGH_MASK) >>
			 SANTOS_PIPE_FRAME_HIGH_SHIFT;
		low = (readl(regs + SANTOS_VDC_PIPEAFRAMEPIXEL) &
		       SANTOS_PIPE_FRAME_LOW_MASK) >>
		       SANTOS_PIPE_FRAME_LOW_SHIFT;
		high2 = (readl(regs + SANTOS_VDC_PIPEAFRAMEHIGH) &
			 SANTOS_PIPE_FRAME_HIGH_MASK) >>
			 SANTOS_PIPE_FRAME_HIGH_SHIFT;
	} while (high1 != high2);

	return ((high1 << 8) | low) & 0x00ffffffU;
}

static bool santos_pvr_vsync_pipe_a_ready(void __iomem *regs)
{
	return (readl(regs + SANTOS_VDC_PIPEACONF) &
		SANTOS_PIPEACONF_ENABLE) != 0;
}

static int santos_pvr_vsync_source_get(void __iomem **regs)
{
	int ret = 0;

	mutex_lock(&santos_irq_lock);
	if (!santos_vsync_regs || READ_ONCE(santos_irq_stopping)) {
		ret = -ENODEV;
	} else {
		*regs = santos_vsync_regs;
		atomic_inc(&santos_vsync_users);
	}
	mutex_unlock(&santos_irq_lock);

	return ret;
}

static void santos_pvr_vsync_source_put(void)
{
	if (atomic_dec_and_test(&santos_vsync_users))
		wake_up(&santos_vsync_users_wq);
}

static int santos_pvr_vsync_enable_state(void)
{
	int ret = 0;

	if (READ_ONCE(santos_irq_stopping))
		return -ENODEV;
	mutex_lock(&santos_vsync_state_lock);
	if (READ_ONCE(santos_irq_stopping)) {
		ret = -ENODEV;
	} else if (!santos_vsync_enabled) {
		santos_vsync_enabled = true;
		santos_vsync_epoch++;
	}
	mutex_unlock(&santos_vsync_state_lock);

	return ret;
}

static void santos_pvr_vsync_disable_state(void)
{
	mutex_lock(&santos_vsync_state_lock);
	santos_vsync_enabled = false;
	/* A disable wakes a waiter, but never counts as a hardware transition. */
	santos_vsync_epoch++;
	mutex_unlock(&santos_vsync_state_lock);
	wake_up_interruptible(&santos_vsync_waitq);
}

static bool santos_pvr_vsync_wait_condition(void __iomem *regs,
					    u32 start_count,
					    unsigned long start_epoch)
{
	if (READ_ONCE(santos_irq_stopping) ||
	    !READ_ONCE(santos_vsync_enabled) ||
	    READ_ONCE(santos_vsync_epoch) != start_epoch)
		return true;

	return santos_pvr_vsync_read_frame_counter(regs) != start_count;
}

static int santos_pvr_vsync_wait(void __iomem *regs, u32 start_count,
				 unsigned long start_epoch)
{
	unsigned long deadline = jiffies + SANTOS_PSB_VSYNC_TIMEOUT_JIFFIES;
	const ktime_t poll_quantum = ktime_set(0, SANTOS_PSB_VSYNC_POLL_NS);

	for (;;) {
		u32 now;
		long ret;

		/* A disable/teardown wakeup is not a VSYNC event. Check lifecycle
		 * state both before and after the MMIO sample so teardown cannot be
		 * reported as a successful scheduling edge. */
		if (READ_ONCE(santos_irq_stopping))
			return -ENODEV;
		if (!READ_ONCE(santos_vsync_enabled) ||
		    READ_ONCE(santos_vsync_epoch) != start_epoch)
			return -EPIPE;
		now = santos_pvr_vsync_read_frame_counter(regs);
		if (READ_ONCE(santos_irq_stopping))
			return -ENODEV;
		if (!READ_ONCE(santos_vsync_enabled) ||
		    READ_ONCE(santos_vsync_epoch) != start_epoch)
			return -EPIPE;
		if (now != start_count)
			return 0;
		if (time_after_eq(jiffies, deadline))
			return -ETIMEDOUT;

		/* No Pipe-A display IRQ is installed by this shell. The hrtimer
		 * quantum bounds the read-only poll at one wakeup per 2 ms while a
		 * vendor WAIT is outstanding; disable/teardown wakes it early. */
		ret = wait_event_interruptible_hrtimeout(
			santos_vsync_waitq,
			santos_pvr_vsync_wait_condition(regs, start_count,
							start_epoch),
			poll_quantum);
		if (ret == -ERESTARTSYS)
			return ret;
		if (ret != 0 && ret != -ETIME)
			return -EIO;
	}
}

static int santos_pvr_psb_vsync_set(struct drm_device *dev, void *arg,
				    struct drm_file *file)
{
	struct santos_psb_vsync_set_arg *a = arg;
	void __iomem *regs = NULL;
	unsigned int mask = a->vsync_operation_mask;
	u32 count;
	unsigned long start_epoch;
	int ret = 0;
	bool source_held = false;

	(void)dev;
	(void)file;

	/* drm_ioctl performs the exact 24-byte copy_from/to_user around this
	 * handler. Reject an empty/unknown operation instead of fake success. */
	if (!mask || (mask & ~SANTOS_PSB_VSYNC_VALID_MASK))
		return -EINVAL;
	/* The current fake-PSB exposes one real display: Pipe A only. */
	if (a->vsync.pipe != SANTOS_PSB_VSYNC_PIPE_A)
		return -EINVAL;

	/* Acquire BAR/source mapping for operations that read registers or check
	 * pipe readiness. Teardown or absent source returns -ENODEV. */
	if (mask & (SANTOS_PSB_GET_VSYNC_COUNT |
		    SANTOS_PSB_VSYNC_WAIT |
		    SANTOS_PSB_VSYNC_ENABLE)) {
		ret = santos_pvr_vsync_source_get(&regs);
		if (ret)
			return ret;
		source_held = true;
	}

	/* Preserve the golden operation ordering: GET precedes WAIT, and WAIT
	 * returns before ENABLE/DISABLE bits in the same request are considered.
	 * GET reads the frame counter without requiring Pipe A to be active,
	 * ensuring requests like DISABLE|GET can complete when the pipe is off. */
	if (mask & SANTOS_PSB_GET_VSYNC_COUNT) {
		count = santos_pvr_vsync_read_frame_counter(regs);
		a->vsync.vsync_count = (int32_t)count;
		a->vsync.timestamp = ktime_get_ns();
	}

	if (mask & SANTOS_PSB_VSYNC_WAIT) {
		if (!santos_pvr_vsync_pipe_a_ready(regs)) {
			ret = -EPIPE;
			a->vsync.timestamp = ktime_get_ns();
			goto out;
		}

		mutex_lock(&santos_vsync_state_lock);
		if (!santos_vsync_enabled) {
			ret = -EPIPE;
			start_epoch = santos_vsync_epoch;
		} else {
			start_epoch = santos_vsync_epoch;
			count = santos_pvr_vsync_read_frame_counter(regs);
		}
		mutex_unlock(&santos_vsync_state_lock);

		if (!ret)
			ret = santos_pvr_vsync_wait(regs, count, start_epoch);
		/* Golden WAIT updates timestamp, but never overwrites vsync_count.
		 * Timestamp is a software observation of a proven transition,
		 * not a hardware-captured vblank timestamp. */
		a->vsync.timestamp = ktime_get_ns();
		goto out;
	}

	if (mask & SANTOS_PSB_VSYNC_ENABLE) {
		if (!santos_pvr_vsync_pipe_a_ready(regs)) {
			ret = -EPIPE;
			goto out;
		}
		ret = santos_pvr_vsync_enable_state();
		if (ret)
			goto out;
	}
	if (mask & SANTOS_PSB_VSYNC_DISABLE)
		santos_pvr_vsync_disable_state();

out:
	if (source_held)
		santos_pvr_vsync_source_put();
	return ret;
}

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
	.name			= "pvrsrvkm",
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
	.driver.suppress_bind_attrs = true,
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
	INIT_DELAYED_WORK(&santos_pending_work, santos_pending_work_fn);
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
	cancel_delayed_work_sync(&santos_pending_work);
	santos_pvr_pdev = NULL;
	santos_pvr_drm = NULL;
	pr_info("santos-pvr-shell: unloaded\n");
}

module_init(santos_pvr_shell_init);
module_exit(santos_pvr_shell_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("Bind-only PVR DRM shell for Cloverview 00:02.0 (M4 S5b)");
MODULE_LICENSE("GPL");
