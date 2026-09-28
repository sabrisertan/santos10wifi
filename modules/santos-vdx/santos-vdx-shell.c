// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos VDX Gate 2 — PSB video UAPI host for the exact 3.4 VA userspace.
 *
 * Presents a DRM node named "pvrsrvkm" (the name the 3.4 pnwdisp driver used
 * and that libva-android's driver-name table expects) on 00:02.0 (8086:08c8)
 * and implements the PSB video ioctls the frozen userspace actually issues.
 *
 * Provenance of the exact numbering (golden 3.4 strace of
 * libva_videodecoder + pvr_drv_video, 2026-09-12):
 *   0x46  EXTENSION         (X=0x06)
 *   0x90  CMDBUF            (X=0x50)  [Gate 3, not implemented here]
 *   0x92  PL_CREATE         (X=0x52)  union size 0x28
 *   0x95  PL_SYNCCPU        (X=0x55)  size 0x10
 *   0x96  PL_WAITIDLE       (X=0x56)  size 0x08
 *   0x9e  VIDEO_GETPARAM    (X=0x5e)  size 0x18
 * Fence numbering from the exact headers:
 *   FENCE_OFFSET=0x59 -> SIGNALED=0x5a FINISH=0x5b UNREF=0x5c FLIP=0x5d.
 *
 * Buffers are real page-backed objects with unique device VAs and mmap
 * support. No MMU programming and no VDX command submission yet: Gate 3
 * wires them into the MSVDX MMU and the command queue.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#define SANTOS_VDX_PCI_VID	0x8086
#define SANTOS_VDX_PCI_DID	0x08c8

#define SANTOS_DRM_COMMAND_BASE	0x40
#define SANTOS_PSB_CMDBUF	0x50
#define SANTOS_PSB_PLACEMENT	0x52
#define SANTOS_PSB_FENCE	0x59
#define SANTOS_PSB_FLIP		0x5d
#define SANTOS_PSB_GETPARAM	0x5e

#define SANTOS_MAX_BOS		512
#define SANTOS_MAX_CTXS		16

struct santos_vdx_bo {
	u32 handle;
	u32 refs;
	u64 size;
	u64 gpu_offset;
	u64 map_handle;
	u32 placement;
	void *cpu;
};

struct santos_vdx_ctx {
	struct list_head list;
	struct file *filp;
	u64 ctx_type;
};

static struct santos_vdx_bo santos_bos[SANTOS_MAX_BOS];
static DEFINE_MUTEX(santos_bos_lock);
static u32 santos_next_handle = 1;
static u64 santos_next_gpu = 0x00100000;
static u64 santos_next_map = 0x40000000;

static LIST_HEAD(santos_ctxs);
static DEFINE_MUTEX(santos_ctxs_lock);

struct santos_psb_extension_rep {
	int32_t exists;
	uint32_t driver_ioctl_offset;
	uint32_t sarea_offset;
	uint32_t major;
	uint32_t minor;
	uint32_t pl;
};

union santos_psb_extension_arg {
	char extension[128];
	struct santos_psb_extension_rep rep;
};

struct santos_ttm_pl_create_req {
	u64 size;
	u32 placement;
	u32 page_alignment;
};

struct santos_ttm_pl_rep {
	u64 gpu_offset;
	u64 bo_size;
	u64 map_handle;
	u32 placement;
	u32 handle;
	u32 sync_object_arg;
	u32 pad64;
};

union santos_ttm_pl_create_arg {
	struct santos_ttm_pl_create_req req;
	struct santos_ttm_pl_rep rep;
};

struct santos_ttm_pl_reference_req {
	u32 handle;
	u32 pad64;
};

union santos_ttm_pl_reference_arg {
	struct santos_ttm_pl_reference_req req;
	struct santos_ttm_pl_rep rep;
};

struct santos_ttm_pl_synccpu_arg {
	u32 handle;
	u32 access_mode;
	u32 op;
	u32 pad64;
};

struct santos_ttm_pl_waitidle_arg {
	u32 handle;
	u32 mode;
};

struct santos_ttm_pl_setstatus_req {
	u32 set_placement;
	u32 clr_placement;
	u32 handle;
	u32 pad64;
};

union santos_ttm_pl_setstatus_arg {
	struct santos_ttm_pl_setstatus_req req;
	struct santos_ttm_pl_rep rep;
};

struct santos_ttm_fence_signaled_req {
	u32 handle;
	u32 fence_type;
	int32_t flush;
	u32 pad64;
};

struct santos_ttm_fence_finish_req {
	u32 handle;
	u32 fence_type;
	u32 mode;
	u32 pad64;
};

struct santos_ttm_fence_rep {
	u32 signaled_types;
	u32 fence_error;
};

union santos_ttm_fence_arg {
	struct santos_ttm_fence_signaled_req signaled;
	struct santos_ttm_fence_finish_req finish;
	struct santos_ttm_fence_rep rep;
};

struct santos_ttm_fence_unref_arg {
	u32 handle;
	u32 pad64;
};

struct santos_video_getparam_arg {
	u64 key;
	u64 arg;
	u64 value;
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

struct santos_displaying_frame {
	u32 buf_handle;
	u32 width;
	u32 height;
	u32 size;
	u32 format;
	u32 luma_stride;
	u32 chroma_u_stride;
	u32 chroma_v_stride;
	u32 luma_offset;
	u32 chroma_u_offset;
	u32 chroma_v_offset;
	u32 reserved;
};

static struct santos_displaying_frame santos_displaying;
static int santos_hdmi_state;
static u32 santos_decoding_err;

#define SANTOS_PSB_EXTENSION_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + 0x06, union santos_psb_extension_arg)
#define SANTOS_PSB_CMDBUF_IOCTL \
	DRM_IOW(SANTOS_DRM_COMMAND_BASE + SANTOS_PSB_CMDBUF, unsigned char[0x30])
#define SANTOS_TTM_PL_CREATE_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + 0x52, union santos_ttm_pl_create_arg)
#define SANTOS_TTM_PL_REFERENCE_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + 0x53, union santos_ttm_pl_reference_arg)
#define SANTOS_TTM_PL_UNREF_IOCTL \
	DRM_IOW(SANTOS_DRM_COMMAND_BASE + 0x54, struct santos_ttm_pl_reference_req)
#define SANTOS_TTM_PL_SYNCCPU_IOCTL \
	DRM_IOW(SANTOS_DRM_COMMAND_BASE + 0x55, struct santos_ttm_pl_synccpu_arg)
#define SANTOS_TTM_PL_WAITIDLE_IOCTL \
	DRM_IOW(SANTOS_DRM_COMMAND_BASE + 0x56, struct santos_ttm_pl_waitidle_arg)
#define SANTOS_TTM_PL_SETSTATUS_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + 0x57, union santos_ttm_pl_setstatus_arg)
#define SANTOS_TTM_PL_CREATE_UB_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + 0x58, union santos_ttm_pl_create_arg)
#define SANTOS_TTM_FENCE_SIGNALED_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + 0x5a, union santos_ttm_fence_arg)
#define SANTOS_TTM_FENCE_FINISH_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + 0x5b, union santos_ttm_fence_arg)
#define SANTOS_TTM_FENCE_UNREF_IOCTL \
	DRM_IOW(SANTOS_DRM_COMMAND_BASE + 0x5c, struct santos_ttm_fence_unref_arg)
#define SANTOS_PSB_GETPARAM_IOCTL \
	DRM_IOWR(SANTOS_DRM_COMMAND_BASE + SANTOS_PSB_GETPARAM, \
		 struct santos_video_getparam_arg)

static struct santos_vdx_bo *santos_bo_find_locked(u32 handle)
{
	int i;

	if (!handle)
		return NULL;
	for (i = 0; i < SANTOS_MAX_BOS; i++)
		if (santos_bos[i].handle == handle)
			return &santos_bos[i];
	return NULL;
}

static void santos_bo_fill_rep(struct santos_vdx_bo *bo,
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

static void santos_bo_free_locked(struct santos_vdx_bo *bo)
{
	if (bo->cpu)
		free_pages_exact(bo->cpu, bo->size);
	memset(bo, 0, sizeof(*bo));
}

static int santos_psb_pl_create(struct drm_device *dev, void *arg,
				struct drm_file *file)
{
	union santos_ttm_pl_create_arg *a = arg;
	struct santos_vdx_bo *bo = NULL;
	u64 size;
	int i;

	(void)dev; (void)file;

	size = (a->req.size + PAGE_SIZE - 1) & PAGE_MASK;
	if (!size || size > (64ULL << 20))
		return -EINVAL;

	mutex_lock(&santos_bos_lock);
	for (i = 0; i < SANTOS_MAX_BOS; i++) {
		if (!santos_bos[i].handle) {
			bo = &santos_bos[i];
			break;
		}
	}
	if (bo) {
		memset(bo, 0, sizeof(*bo));
		bo->cpu = alloc_pages_exact(size, GFP_KERNEL | __GFP_ZERO);
		if (!bo->cpu) {
			mutex_unlock(&santos_bos_lock);
			return -ENOMEM;
		}
		bo->handle = santos_next_handle++;
		bo->refs = 1;
		bo->size = size;
		bo->placement = a->req.placement;
		bo->gpu_offset = santos_next_gpu;
		bo->map_handle = santos_next_map;
		santos_next_gpu += size;
		santos_next_map += 0x100000;
		santos_bo_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_bos_lock);

	if (!bo)
		return -ENOMEM;

	pr_info("santos-vdx: PL_CREATE size=0x%llx place=0x%x -> h=%u gpu=0x%llx map=0x%llx\n",
		(unsigned long long)a->rep.bo_size, a->rep.placement,
		a->rep.handle, (unsigned long long)a->rep.gpu_offset,
		(unsigned long long)a->rep.map_handle);
	return 0;
}

static int santos_psb_pl_reference(struct drm_device *dev, void *arg,
				   struct drm_file *file)
{
	union santos_ttm_pl_reference_arg *a = arg;
	struct santos_vdx_bo *bo;

	(void)dev; (void)file;

	mutex_lock(&santos_bos_lock);
	bo = santos_bo_find_locked(a->req.handle);
	if (bo) {
		bo->refs++;
		santos_bo_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_bos_lock);
	return bo ? 0 : -EINVAL;
}

static int santos_psb_pl_unref(struct drm_device *dev, void *arg,
			       struct drm_file *file)
{
	struct santos_ttm_pl_reference_req *a = arg;
	struct santos_vdx_bo *bo;

	(void)dev; (void)file;

	mutex_lock(&santos_bos_lock);
	bo = santos_bo_find_locked(a->handle);
	if (bo && --bo->refs == 0)
		santos_bo_free_locked(bo);
	mutex_unlock(&santos_bos_lock);
	return 0;
}

static int santos_psb_pl_synccpu(struct drm_device *dev, void *arg,
				 struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	return 0;
}

static int santos_psb_pl_waitidle(struct drm_device *dev, void *arg,
				  struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	return 0;
}

static int santos_psb_pl_setstatus(struct drm_device *dev, void *arg,
				   struct drm_file *file)
{
	union santos_ttm_pl_setstatus_arg *a = arg;
	struct santos_vdx_bo *bo;

	(void)dev; (void)file;

	mutex_lock(&santos_bos_lock);
	bo = santos_bo_find_locked(a->req.handle);
	if (bo) {
		bo->placement |= a->req.set_placement;
		bo->placement &= ~a->req.clr_placement;
		santos_bo_fill_rep(bo, &a->rep);
	}
	mutex_unlock(&santos_bos_lock);
	return bo ? 0 : -EINVAL;
}

static int santos_psb_fence_signaled(struct drm_device *dev, void *arg,
				     struct drm_file *file)
{
	union santos_ttm_fence_arg *a = arg;

	(void)dev; (void)file;
	a->rep.signaled_types = a->signaled.fence_type;
	a->rep.fence_error = 0;
	return 0;
}

static int santos_psb_fence_finish(struct drm_device *dev, void *arg,
				   struct drm_file *file)
{
	union santos_ttm_fence_arg *a = arg;

	(void)dev; (void)file;
	a->rep.signaled_types = a->finish.fence_type;
	a->rep.fence_error = 0;
	return 0;
}

static int santos_psb_fence_unref(struct drm_device *dev, void *arg,
				  struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	return 0;
}

static int santos_psb_cmdbuf(struct drm_device *dev, void *arg,
			     struct drm_file *file)
{
	(void)dev; (void)arg; (void)file;
	pr_info("santos-vdx: CMDBUF (Gate 3, not wired)\n");
	return -EINVAL;
}

static void santos_ctxs_remove(struct file *filp)
{
	struct santos_vdx_ctx *ctx, *tmp;

	mutex_lock(&santos_ctxs_lock);
	list_for_each_entry_safe(ctx, tmp, &santos_ctxs, list) {
		if (ctx->filp == filp) {
			list_del(&ctx->list);
			kfree(ctx);
		}
	}
	mutex_unlock(&santos_ctxs_lock);
}

static int santos_psb_getparam(struct drm_device *dev, void *arg,
			       struct drm_file *file)
{
	struct santos_video_getparam_arg *a = arg;
	void __user *uval = (void __user *)(unsigned long)a->value;
	void __user *uarg = (void __user *)(unsigned long)a->arg;
	struct santos_vdx_ctx *ctx;
	u32 tmp32 = 0;
	u64 ctx_type = 0;
	int ret = 0;

	(void)dev;

	switch (a->key) {
	case SANTOS_LNC_VIDEO_DEVICE_INFO:
		tmp32 = ((u32)SANTOS_VDX_PCI_DID << 16);
		if (copy_to_user(uval, &tmp32, sizeof(tmp32)))
			ret = -EFAULT;
		break;
	case SANTOS_LNC_VIDEO_GETPARAM_IMR_INFO: {
		u32 imr[2] = { 0, 0 };
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
		mutex_lock(&santos_ctxs_lock);
		list_add(&ctx->list, &santos_ctxs);
		mutex_unlock(&santos_ctxs_lock);
		pr_info("santos-vdx: NEW_CONTEXT type=0x%llx\n",
			(unsigned long long)ctx_type);
		break;
	case SANTOS_IMG_VIDEO_RM_CONTEXT:
		santos_ctxs_remove(file->filp);
		break;
	case SANTOS_IMG_VIDEO_UPDATE_CONTEXT:
		if (copy_from_user(&ctx_type, uval, sizeof(ctx_type))) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&santos_ctxs_lock);
		list_for_each_entry(ctx, &santos_ctxs, list) {
			if (ctx->filp == file->filp) {
				ctx->ctx_type = ctx_type;
				break;
			}
		}
		mutex_unlock(&santos_ctxs_lock);
		break;
	case SANTOS_IMG_VIDEO_DECODE_STATUS:
		if (copy_to_user(uval, &santos_decoding_err,
				 sizeof(santos_decoding_err)))
			ret = -EFAULT;
		break;
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
		u32 entry, count = 0;
		struct santos_vdx_ctx *c;

		if (copy_from_user(&entry, uarg, sizeof(entry))) {
			ret = -EFAULT;
			break;
		}
		mutex_lock(&santos_ctxs_lock);
		list_for_each_entry(c, &santos_ctxs, list)
			if ((c->ctx_type & 0xff) == entry)
				count++;
		mutex_unlock(&santos_ctxs_lock);
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
		pr_info("santos-vdx: getparam unknown key %llu\n",
			(unsigned long long)a->key);
		ret = -EFAULT;
		break;
	}
	return ret;
}

static int santos_psb_extension(struct drm_device *dev, void *arg,
				struct drm_file *file)
{
	union santos_psb_extension_arg *a = arg;
	u32 off = 0;

	(void)dev; (void)file;

	if (!strcmp(a->extension, "psb_ttm_placement_alphadrop"))
		off = SANTOS_PSB_PLACEMENT;
	else if (!strcmp(a->extension, "psb_ttm_fence_alphadrop"))
		off = SANTOS_PSB_FENCE;
	else if (!strcmp(a->extension, "psb_ttm_execbuf_alphadrop"))
		off = SANTOS_PSB_CMDBUF;
	else if (!strcmp(a->extension, "psb_page_flipping_alphadrop"))
		off = SANTOS_PSB_FLIP;
	else if (!strcmp(a->extension, "lnc_video_getparam"))
		off = SANTOS_PSB_GETPARAM;

	if (!off) {
		a->rep.exists = 0;
		return 0;
	}
	a->rep.exists = 1;
	a->rep.driver_ioctl_offset = off;
	a->rep.sarea_offset = 0;
	a->rep.major = 1;
	a->rep.minor = 0;
	a->rep.pl = 0;
	return 0;
}

static int santos_vdx_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long pgoff = vma->vm_pgoff;
	unsigned long len = vma->vm_end - vma->vm_start;
	struct santos_vdx_bo *bo = NULL;
	unsigned long off;
	int i;

	mutex_lock(&santos_bos_lock);
	for (i = 0; i < SANTOS_MAX_BOS; i++) {
		if (santos_bos[i].handle &&
		    (santos_bos[i].map_handle >> PAGE_SHIFT) == pgoff) {
			bo = &santos_bos[i];
			break;
		}
	}
	if (!bo || len > bo->size) {
		mutex_unlock(&santos_bos_lock);
		return -EINVAL;
	}

	for (off = 0; off < len; off += PAGE_SIZE) {
		unsigned long pfn = page_to_pfn(virt_to_page(bo->cpu + off));
		if (remap_pfn_range(vma, vma->vm_start + off, pfn,
				    PAGE_SIZE, vma->vm_page_prot)) {
			mutex_unlock(&santos_bos_lock);
			return -EAGAIN;
		}
	}
	mutex_unlock(&santos_bos_lock);
	pr_info("santos-vdx: mmap h=%u off=0x%lx len=0x%lx\n",
		bo->handle, pgoff << PAGE_SHIFT, len);
	return 0;
}

static int santos_vdx_open(struct drm_device *dev, struct drm_file *file)
{
	return 0;
}

static void santos_vdx_postclose(struct drm_device *dev, struct drm_file *file)
{
	(void)dev;
	santos_ctxs_remove(file->filp);
}

static const struct file_operations santos_vdx_fops = {
	.owner		= THIS_MODULE,
	.fop_flags	= FOP_UNSIGNED_OFFSET,
	.open		= drm_open,
	.release	= drm_release,
	.unlocked_ioctl	= drm_ioctl,
	.poll		= drm_poll,
	.read		= drm_read,
	.mmap		= santos_vdx_mmap,
	.llseek		= noop_llseek,
};

static const struct drm_ioctl_desc santos_vdx_ioctls[] = {
	[0x06] = { .cmd = SANTOS_PSB_EXTENSION_IOCTL,
	  .flags = 0, .func = santos_psb_extension,
	  .name = "PSB_EXTENSION" },
	[0x50] = { .cmd = SANTOS_PSB_CMDBUF_IOCTL,
	  .flags = 0, .func = santos_psb_cmdbuf,
	  .name = "PSB_CMDBUF" },
	[0x52] = { .cmd = SANTOS_TTM_PL_CREATE_IOCTL,
	  .flags = 0, .func = santos_psb_pl_create,
	  .name = "TTM_PL_CREATE" },
	[0x53] = { .cmd = SANTOS_TTM_PL_REFERENCE_IOCTL,
	  .flags = 0, .func = santos_psb_pl_reference,
	  .name = "TTM_PL_REFERENCE" },
	[0x54] = { .cmd = SANTOS_TTM_PL_UNREF_IOCTL,
	  .flags = 0, .func = santos_psb_pl_unref,
	  .name = "TTM_PL_UNREF" },
	[0x55] = { .cmd = SANTOS_TTM_PL_SYNCCPU_IOCTL,
	  .flags = 0, .func = santos_psb_pl_synccpu,
	  .name = "TTM_PL_SYNCCPU" },
	[0x56] = { .cmd = SANTOS_TTM_PL_WAITIDLE_IOCTL,
	  .flags = 0, .func = santos_psb_pl_waitidle,
	  .name = "TTM_PL_WAITIDLE" },
	[0x57] = { .cmd = SANTOS_TTM_PL_SETSTATUS_IOCTL,
	  .flags = 0, .func = santos_psb_pl_setstatus,
	  .name = "TTM_PL_SETSTATUS" },
	[0x58] = { .cmd = SANTOS_TTM_PL_CREATE_UB_IOCTL,
	  .flags = 0, .func = santos_psb_pl_create,
	  .name = "TTM_PL_CREATE_UB" },
	[0x5a] = { .cmd = SANTOS_TTM_FENCE_SIGNALED_IOCTL,
	  .flags = 0, .func = santos_psb_fence_signaled,
	  .name = "TTM_FENCE_SIGNALED" },
	[0x5b] = { .cmd = SANTOS_TTM_FENCE_FINISH_IOCTL,
	  .flags = 0, .func = santos_psb_fence_finish,
	  .name = "TTM_FENCE_FINISH" },
	[0x5c] = { .cmd = SANTOS_TTM_FENCE_UNREF_IOCTL,
	  .flags = 0, .func = santos_psb_fence_unref,
	  .name = "TTM_FENCE_UNREF" },
	[0x5e] = { .cmd = SANTOS_PSB_GETPARAM_IOCTL,
	  .flags = 0, .func = santos_psb_getparam,
	  .name = "PSB_VIDEO_GETPARAM" },
};
static_assert(ARRAY_SIZE(santos_vdx_ioctls) == 0x5f);

static const struct drm_driver santos_vdx_drm_driver = {
	.driver_features	= 0,
	.name			= "pvrsrvkm",
	.desc			= "Santos VDX PSB video UAPI host (Gate 2)",
	.major			= 1,
	.minor			= 0,
	.patchlevel		= 0,
	.fops			= &santos_vdx_fops,
	.open			= santos_vdx_open,
	.postclose		= santos_vdx_postclose,
	.ioctls			= santos_vdx_ioctls,
	.num_ioctls		= ARRAY_SIZE(santos_vdx_ioctls),
};

static struct pci_dev *santos_vdx_pdev;
static struct drm_device *santos_vdx_drm;

static int santos_vdx_probe(struct pci_dev *pdev,
			    const struct pci_device_id *id)
{
	struct drm_device *drm;
	int ret;

	pr_info("santos-vdx: probe 00:02.0 (PSB video UAPI host)\n");

	drm = drm_dev_alloc(&santos_vdx_drm_driver, &pdev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	pci_set_drvdata(pdev, drm);
	santos_vdx_pdev = pdev;
	santos_vdx_drm = drm;

	ret = drm_dev_register(drm, 0);
	if (ret) {
		pci_set_drvdata(pdev, NULL);
		santos_vdx_pdev = NULL;
		santos_vdx_drm = NULL;
		drm_dev_put(drm);
		return ret;
	}
	pr_info("santos-vdx: card registered as 'pvrsrvkm'\n");
	return 0;
}

static void santos_vdx_remove(struct pci_dev *pdev)
{
	struct drm_device *drm = pci_get_drvdata(pdev);
	int i;

	if (!drm)
		return;
	drm_dev_unplug(drm);
	drm_dev_unregister(drm);
	pci_set_drvdata(pdev, NULL);
	santos_vdx_pdev = NULL;
	santos_vdx_drm = NULL;
	drm_dev_put(drm);

	mutex_lock(&santos_bos_lock);
	for (i = 0; i < SANTOS_MAX_BOS; i++)
		if (santos_bos[i].handle)
			santos_bo_free_locked(&santos_bos[i]);
	mutex_unlock(&santos_bos_lock);
	pr_info("santos-vdx: removed\n");
}

static const struct pci_device_id santos_vdx_ids[] = {
	{ PCI_DEVICE(SANTOS_VDX_PCI_VID, SANTOS_VDX_PCI_DID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, santos_vdx_ids);

static struct pci_driver santos_vdx_pci_driver = {
	.name		= "santos-vdx",
	.id_table	= santos_vdx_ids,
	.probe		= santos_vdx_probe,
	.remove		= santos_vdx_remove,
};

static int __init santos_vdx_shell_init(void)
{
	pr_info("santos-vdx: Gate 2 PSB video UAPI host loading\n");
	return pci_register_driver(&santos_vdx_pci_driver);
}

static void __exit santos_vdx_shell_exit(void)
{
	pci_unregister_driver(&santos_vdx_pci_driver);
	pr_info("santos-vdx: unloaded\n");
}

module_init(santos_vdx_shell_init);
module_exit(santos_vdx_shell_exit);

MODULE_AUTHOR("Santos10wifi VDX7 phase");
MODULE_DESCRIPTION("Santos VDX Gate 2 PSB video UAPI host");
MODULE_LICENSE("GPL");
