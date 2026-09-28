// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S3 skeleton stub for the Android sync_framework integration.
 *
 * The golden pvr_sync.c implements PVR sync objects on top of the old
 * Android sync_timeline/sync_pt API, which does not exist in Linux 7.2
 * (dma_fence/sync_file replaced it; CONFIG_SYNC_FILE=y is present).
 * The real dma_fence port lands in S5, following the 1.17 C-oracle
 * (which already migrated this file).
 *
 * This stub provides linkable definitions with signatures copied
 * verbatim from pvr_sync.h so the exact-1.12 core links in the
 * no-hardware skeleton phase. Every stub reports "not supported";
 * none of them can run because nothing probes hardware yet.
 * NOT ABI: internal linkage only.
 */
#include "servicesint.h"
#include "pvr_debug.h"
#include <drm/drm_device.h>
#include "psb_powermgmt.h"
#include <linux/history_record.h>
#include <linux/anon_inodes.h>
#include <linux/poll.h>
#include <linux/string.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "pvr_sync.h"
#include <sw_sync.h>

int PVRSyncDeviceInit(void)
{
	/* L11 M3: register the real provider when the santos sync core is
	 * present; stays dormant (legacy stub behaviour) otherwise. */
	return PVRSyncProviderInit();
}

void PVRSyncDeviceDeInit(void)
{
	PVRSyncProviderDeInit();
}

void PVRSyncUpdateAllSyncs(void)
{
	/* M5: authoritative completion point (MISR workqueue tail). */
	PVRSyncProviderUpdateAllSyncs();
}

PVRSRV_ERROR
PVRSyncPatchCCBKickSyncInfos(IMG_HANDLE ahSyncs[SGX_MAX_SRC_SYNCS_TA],
		      PVRSRV_DEVICE_SYNC_OBJECT asDevSyncs[SGX_MAX_SRC_SYNCS_TA],
							 IMG_UINT32 *pui32NumSrcSyncs)
{
	PVR_UNREFERENCED_PARAMETER(ahSyncs);
	PVR_UNREFERENCED_PARAMETER(asDevSyncs);
	/* A144: /dev/pvr_sync hands out already-signaled dma-fence stubs, so a
	 * kick that lists source fences has nothing real to wait on. Dropping
	 * the dependency set (count 0) keeps the kick path alive without
	 * programming bogus device sync objects into the CCB; the real
	 * kick-attached fence port lands with the dma_fence work. */
	*pui32NumSrcSyncs = 0;
	return PVRSRV_OK;
}

PVRSRV_ERROR
PVRSyncFencesToSyncInfos(PVRSRV_KERNEL_SYNC_INFO *apsSyncs[],
						 IMG_UINT32 *pui32NumSyncs,
						 struct sync_fence *apsFence[SGX_MAX_SRC_SYNCS_TA])
{
	PVR_UNREFERENCED_PARAMETER(apsSyncs);
	PVR_UNREFERENCED_PARAMETER(apsFence);
	/* A146: the swap path hands in fence ids that this port cannot resolve
	 * to kernel sync infos (all /dev/pvr_sync fences are already-signaled
	 * stubs). Report zero dependencies so PVRSRVSwapToDCBuffer2KM proceeds;
	 * DC_NOHW does not scan out, so there is nothing to serialize against. */
	*pui32NumSyncs = 0;
	return PVRSRV_OK;
}

/*
 * A147: minimal sw_sync/sync_fence objects with the OLD Android sync UAPI.
 *
 * The vendor UM (gralloc.clovertrail) is built against the pre-sync_file
 * libsync, which drives fences with SYNC_IOC_MERGE ('>',1) and friends.
 * 7.2's sync_file only answers the modern 's' ioctls, so every fence fd in
 * this port is instead a private anon-inode file that implements the old
 * '>' UAPI (WAIT/MERGE/FENCE_INFO). All fences are already signaled; the
 * MERGE ioctl returns a fresh file. Timelines/sync points are opaque tokens.
 */
#define SANTOS_SYNC_IOC_MAGIC '>'

struct santos_sync_merge_data {
	__s32	fd2;
	char	name[32];
	__s32	fence;
};

struct santos_sync_fence_info_data {
	__u32	len;
	char	name[32];
	__s32	status;
};

#define SANTOS_SYNC_IOC_WAIT \
	_IOW(SANTOS_SYNC_IOC_MAGIC, 0, __s32)
#define SANTOS_SYNC_IOC_MERGE \
	_IOWR(SANTOS_SYNC_IOC_MAGIC, 1, struct santos_sync_merge_data)
#define SANTOS_SYNC_IOC_FENCE_INFO \
	_IOWR(SANTOS_SYNC_IOC_MAGIC, 2, struct santos_sync_fence_info_data)

struct santos_sync_fence {
	char name[32];
};

struct file *SantosPVRSyncNewFenceFile(const char *szName);

static const struct file_operations santos_sync_fence_fops;

static int santos_sync_fence_release(struct inode *inode, struct file *file)
{
	PVR_UNREFERENCED_PARAMETER(inode);
	kfree(file->private_data);
	return 0;
}

static __poll_t santos_sync_fence_poll(struct file *file,
				       struct poll_table_struct *wait)
{
	PVR_UNREFERENCED_PARAMETER(file);
	PVR_UNREFERENCED_PARAMETER(wait);
	return EPOLLIN | EPOLLRDNORM;
}

static long santos_sync_fence_ioctl_merge(struct file *file,
					  unsigned long arg)
{
	struct santos_sync_merge_data sData;
	struct santos_sync_fence *psNew;
	struct file *psFile;
	int fd;

	PVR_UNREFERENCED_PARAMETER(file);

	if (copy_from_user(&sData, (void __user *)arg, sizeof(sData)))
		return -EFAULT;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	psNew = kzalloc(sizeof(*psNew), GFP_KERNEL);
	if (!psNew) {
		put_unused_fd(fd);
		return -ENOMEM;
	}
	strscpy(psNew->name, sData.name, sizeof(psNew->name));

	psFile = anon_inode_getfile("pvr_sync_fence", &santos_sync_fence_fops,
				    psNew, O_RDWR);
	if (IS_ERR(psFile)) {
		kfree(psNew);
		put_unused_fd(fd);
		return PTR_ERR(psFile);
	}

	sData.fence = fd;
	if (copy_to_user((void __user *)arg, &sData, sizeof(sData))) {
		fput(psFile);
		put_unused_fd(fd);
		return -EFAULT;
	}

	fd_install(fd, psFile);
	return 0;
}

static long santos_sync_fence_ioctl_info(struct file *file, unsigned long arg)
{
	struct santos_sync_fence *psFence = file->private_data;
	struct santos_sync_fence_info_data sInfo;
	__u32 ui32UserLen;

	if (get_user(ui32UserLen, (__u32 __user *)arg))
		return -EFAULT;
	if (ui32UserLen < sizeof(sInfo))
		return -EINVAL;

	memset(&sInfo, 0, sizeof(sInfo));
	sInfo.len = sizeof(sInfo);
	strscpy(sInfo.name, psFence->name, sizeof(sInfo.name));
	sInfo.status = 1; /* signaled */

	if (copy_to_user((void __user *)arg, &sInfo, sizeof(sInfo)))
		return -EFAULT;
	return 0;
}

static long santos_sync_fence_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	switch (cmd) {
	case SANTOS_SYNC_IOC_WAIT:
		return 0; /* already signaled */
	case SANTOS_SYNC_IOC_MERGE:
		return santos_sync_fence_ioctl_merge(file, arg);
	case SANTOS_SYNC_IOC_FENCE_INFO:
		return santos_sync_fence_ioctl_info(file, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations santos_sync_fence_fops = {
	.owner = THIS_MODULE,
	.release = santos_sync_fence_release,
	.poll = santos_sync_fence_poll,
	.unlocked_ioctl = santos_sync_fence_ioctl,
	.compat_ioctl = santos_sync_fence_ioctl,
	.llseek = noop_llseek,
};

/*
 * Factory shared with the shell's /dev/pvr_sync device (symbol_get).
 * Returns a file whose fd answers the old sync UAPI; release frees the
 * private state.
 */
struct file *SantosPVRSyncNewFenceFile(const char *szName)
{
	struct santos_sync_fence *psFence;
	struct file *psFile;

	psFence = kzalloc(sizeof(*psFence), GFP_KERNEL);
	if (!psFence)
		return ERR_PTR(-ENOMEM);

	strscpy(psFence->name, szName ? szName : "pvr_sync",
		sizeof(psFence->name));

	psFile = anon_inode_getfile("pvr_sync_fence", &santos_sync_fence_fops,
				    psFence, O_RDWR);
	if (IS_ERR(psFile)) {
		kfree(psFence);
		return psFile;
	}
	return psFile;
}
EXPORT_SYMBOL_GPL(SantosPVRSyncNewFenceFile);

struct sw_sync_timeline {
	IMG_UINT32 ui32Unused;
};

struct sync_pt {
	IMG_UINT32 ui32Unused;
};

struct sw_sync_timeline *sw_sync_timeline_create(const char *name)
{
	PVR_UNREFERENCED_PARAMETER(name);
	return kzalloc(sizeof(struct sw_sync_timeline), GFP_KERNEL);
}

void sw_sync_timeline_inc(struct sw_sync_timeline *obj, __u32 value)
{
	PVR_UNREFERENCED_PARAMETER(obj);
	PVR_UNREFERENCED_PARAMETER(value);
}

void sync_timeline_destroy(struct sw_sync_timeline *obj)
{
	kfree(obj);
}

struct sync_pt *sw_sync_pt_create(struct sw_sync_timeline *obj, __u32 value)
{
	PVR_UNREFERENCED_PARAMETER(obj);
	PVR_UNREFERENCED_PARAMETER(value);
	return kzalloc(sizeof(struct sync_pt), GFP_KERNEL);
}

void sync_pt_free(struct sync_pt *pt)
{
	kfree(pt);
}

struct sync_fence *sync_fence_create(const char *name, struct sync_pt *pt)
{
	struct file *psFile;

	psFile = SantosPVRSyncNewFenceFile(name);
	if (IS_ERR(psFile))
		/* Golden contract (A147 kernel drivers/base/sync.c: on failure the
		 * caller keeps ownership of pt and frees it). */
		return IMG_NULL;
	kfree(pt);	/* success: stub fence does not retain the dummy pt */
	return (struct sync_fence *)psFile;
}

void sync_fence_put(struct sync_fence *fence)
{
	if (fence)
		fput((struct file *)fence);
}

int sync_fence_install(struct sync_fence *fence, int fd)
{
	if (!fence || fd < 0)
		return -EINVAL;
	fd_install(fd, (struct file *)fence);
	return 0;
}

/* PSB power/IRQ interop stubs (S3 skeleton; real island control in S5). */
#include <linux/pci.h>

/*
 * A125: golden psb_powermgmt.c island-state semantics (software subset).
 *
 * Golden psb_driver_load's ospm_power_init() assumes all islands are ON
 * initially (g_hw_power_status_mask = OSPM_ALL_ISLANDS) and only clears bits
 * in the island-down paths; ospm_power_using_hw_begin() then follows an
 * already-on fast path. The Santos port has no island power-down path, and
 * A122/A123 measured the graphics island as accessible (SGX MMIO survives
 * after the early WA), so the tracked state stays ON. begin() therefore
 * returns success only when the requested island is tracked ON; it never
 * fabricates a power-up it cannot verify (APM island-up remains unimplemented
 * and unverified on 7.2).
 */
#define SANTOS_OSPM_ALL_ISLANDS (OSPM_GRAPHICS_ISLAND | OSPM_VIDEO_DEC_ISLAND | \
				 OSPM_VIDEO_ENC_ISLAND | OSPM_GL3_CACHE_ISLAND | \
				 OSPM_DISPLAY_ISLAND)
static u32 g_santos_hw_power_status_mask = SANTOS_OSPM_ALL_ISLANDS;
static atomic_t g_santos_display_access_count = ATOMIC_INIT(0);
static atomic_t g_santos_graphics_access_count = ATOMIC_INIT(0);

bool ospm_power_using_hw_begin(int hw_island, UHBUsage usage)
{
	u32 island = (u32)hw_island;

	PVR_UNREFERENCED_PARAMETER(usage);

	if (!(island & (OSPM_GRAPHICS_ISLAND | OSPM_DISPLAY_ISLAND |
			OSPM_GL3_CACHE_ISLAND)))
		return false;

	if ((g_santos_hw_power_status_mask & island) != island) {
		printk(KERN_WARNING "santos-ospm: begin(0x%x): island tracked off; no port power-up path\n",
		       island);
		return false;
	}

	if (island & OSPM_DISPLAY_ISLAND)
		atomic_inc(&g_santos_display_access_count);
	if (island & OSPM_GRAPHICS_ISLAND)
		atomic_inc(&g_santos_graphics_access_count);

	return true;
}

void ospm_power_using_hw_end(int hw_island)
{
	u32 island = (u32)hw_island;

	if (!(island & (OSPM_GRAPHICS_ISLAND | OSPM_DISPLAY_ISLAND |
			OSPM_GL3_CACHE_ISLAND)))
		return;

	if (island & OSPM_DISPLAY_ISLAND)
		atomic_dec(&g_santos_display_access_count);
	if (island & OSPM_GRAPHICS_ISLAND)
		atomic_dec(&g_santos_graphics_access_count);
}

bool ospm_power_is_hw_on(int hw_islands)
{
	u32 islands = (u32)hw_islands;

	return (g_santos_hw_power_status_mask & islands) == islands;
}

/* No island power-down path in the port: the tracked mask intentionally stays
 * ON so it matches the measured hardware state (islands are never gated by
 * this port). The actual island-up/down MMIO land with the platform power
 * milestone; these functions must not claim power that is not tracked. */
void ospm_power_island_down(int hw_islands)
{
	PVR_UNREFERENCED_PARAMETER(hw_islands);
}

int ospm_power_island_up(int hw_islands)
{
	PVR_UNREFERENCED_PARAMETER(hw_islands);
	return -ENOSYS;
}

void ospm_power_graphics_island_down(int hw_islands)
{
	PVR_UNREFERENCED_PARAMETER(hw_islands);
}

void ospm_power_graphics_island_up(int hw_islands)
{
	PVR_UNREFERENCED_PARAMETER(hw_islands);
}

void psb_irq_uninstall_islands(struct drm_device *dev, int hw_islands)
{
	PVR_UNREFERENCED_PARAMETER(dev);
	PVR_UNREFERENCED_PARAMETER(hw_islands);
}

/* PSB-provided globals for the skeleton (S5 wires real ones). */
struct drm_device *g_drm_dev = NULL;

struct saved_history_record *get_new_history_record(void)
{
	return NULL;
}
