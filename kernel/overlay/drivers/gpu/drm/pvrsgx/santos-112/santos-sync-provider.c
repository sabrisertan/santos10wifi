// SPDX-License-Identifier: GPL-2.0-only
/*
 * santos-sync-provider.c - L11 M3 provider bridge for the santos sync core.
 *
 * The M1 core (santos-sync-core.ko, /dev/pvr_sync_core) owns the fence fds,
 * the timeline and the userspace ALLOC/CREATE ordering.  It deliberately
 * knows nothing about the PVR driver: an alloc fd only carries an opaque
 * provider token that this module associates with the *real* SGX completion
 * object of a submitted transfer.
 *
 * Real object and completion rule
 * ------------------------------
 * The object bound here is a dedicated PVRSRV_KERNEL_SYNC_INFO (the "fence
 * syncinfo"), allocated by this provider in the services device memory
 * context.  It is placed into the transfer command as an extra destination
 * sync (device sync object + write op taken) exactly the way the golden
 * 3.4/1.12 pvr_sync.c does it, so the GPU/UKernel completes it when the
 * transfer finishes and the driver's own completion rule applies:
 *
 *     complete  ==  WriteOpsComplete >= WriteOpsPending
 *
 * That is the same state the SGX driver trusts for its own destination
 * syncs; the token is not a synthetic counter.  is_complete() only reads
 * that state, never signals, and never sleeps (it runs under the core's
 * fence-list spinlock from the MISR workqueue tail).
 *
 * Lifetime invariant
 * ------------------
 * One wrapper (struct santos_pvr_fence_syncinfo) owns exactly one syncinfo
 * from allocation until the core has released the token AND the GPU no
 * longer references the object:
 *
 *   prepare:  wrapper + syncinfo allocated, wrapper on g_fenceSyncInfos,
 *             device sync object copied into the CCB, write op taken.
 *   commit:   santos_sync_alloc_bind(file, wrapper, BOUND_PENDING) accepts
 *             the token; the core then holds one provider module reference
 *             and one outstanding token until release().
 *   abort:    no token was accepted (schedule failed or was retried); the
 *             write op was already rolled back by the caller, so the worker
 *             frees the object as soon as it is not in use.
 *   release:  called exactly once by the core (alloc fd close before CREATE,
 *             or final fence release).  It only marks + queues; the worker
 *             runs in process context, takes the bridge lock and calls
 *             PVRSRVReleaseSyncInfoKM() once !PVRSyncIsSyncInfoInUse() (or
 *             once the device aborted the work).  The callback itself never
 *             sleeps, matching the golden defer-free pattern.
 *
 * Nothing here keeps a raw pointer to somebody else's syncinfo: the only
 * token is the provider-owned object, so no foreign lifetime can move under
 * us.  The core pins this module per accepted token (ops.owner), therefore
 * the module cannot be unloaded while any wrapper is still referenced by a
 * fence or alloc fd.
 *
 * Activation
 * ----------
 * The core is an optional module.  This provider resolves its exported
 * symbols with symbol_get() at device init (and lazily at the first
 * transfer if the core was loaded later).  Without the core everything
 * keeps the accepted always-signaled stub behaviour.  The core must be
 * loaded before a core alloc fd can exist; the per-symbol module references
 * are dropped in PVRSyncProviderDeInit().
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>

#include "servicesint.h"
#include "pvr_bridge_km.h"
#include "srvkm.h"
#include "device.h"
#include "perproc.h"
#include "osfunc.h"
#include "mutex.h"
#include "lock.h"
#include "pvr_sync.h"
#include "santos-sync-core.h"

/* KBUILD_MODNAME of the core module (santos-sync-core.ko). */
#define SANTOS_SYNC_CORE_MODULE	"santos_sync_core"
#define SANTOS_SYNC_PROVIDER_NAME "santos-sync-provider"

/*
 * Core API resolved with symbol_get().  Keeping the core optional means the
 * provider module still loads (and the stub still runs) when the M1 core is
 * not deployed; the pre-existing production configuration is untouched.
 */
static int (*g_pfnSetProviderOps)(const struct santos_sync_provider_ops *ops);
static int (*g_pfnClearProviderOps)(void);
static int (*g_pfnAllocBind)(struct file *file, void *syncinfo, int state);
static int (*g_pfnAllocClaim)(struct file *file);
static void (*g_pfnAllocUnclaim)(struct file *file);
static int (*g_pfnUpdateAll)(void);
static bool g_symbolsHeld;

/* True while the core accepted this provider's ops. */
static bool g_active;

/*
 * One real SGX completion object plus the provider-side state the core
 * contract needs (released = the core dropped the token; aborted = the
 * device reset/teardown means no completion will ever arrive).
 */
struct santos_pvr_fence_syncinfo {
	struct list_head link;
	PVRSRV_KERNEL_SYNC_INFO *psBase;
	bool released;
	bool aborted;
};

static LIST_HEAD(g_fenceSyncInfos);
static DEFINE_SPINLOCK(g_fenceSyncInfosLock);

/* Set while a released object still waits for completion or the worker. */
static atomic_t g_releasePending = ATOMIC_INIT(0);
static struct workqueue_struct *g_releaseQueue;
static struct work_struct g_releaseWork;

/* Device-global services state used to allocate the fence syncinfos. */
static bool g_servicesConnected;
static IMG_HANDLE g_servicesDevCookie;
static IMG_HANDLE g_servicesDevMemContext;

/* ------------------------------------------------------------------ */
/* ownership helpers                                                   */
/* ------------------------------------------------------------------ */

/*
 * Golden pvr_sync.c PVRSyncIsSyncInfoInUse(): true while any op class is
 * still pending.  The shared memory is device-coherent; the values may be
 * written by the GPU concurrently, which is fine for a "wait until idle"
 * test.
 */
static bool santos_sync_provider_syncinfo_in_use(PVRSRV_KERNEL_SYNC_INFO *psSyncInfo)
{
	PVRSRV_SYNC_DATA *psSyncData = psSyncInfo->psSyncData;

	return !(psSyncData->ui32WriteOpsPending == psSyncData->ui32WriteOpsComplete &&
		 psSyncData->ui32ReadOpsPending == psSyncData->ui32ReadOpsComplete &&
		 psSyncData->ui32ReadOps2Pending == psSyncData->ui32ReadOps2Complete);
}

/* Queue the defer-free worker; safe from the core release callback context. */
static void santos_sync_provider_schedule_release(void)
{
	atomic_set(&g_releasePending, 1);
	if (g_releaseQueue)
		queue_work(g_releaseQueue, &g_releaseWork);
}

/*
 * Provider release: the core guarantees exactly one call after the token
 * stops being reachable from userspace.  No sleeping here (contract); the
 * real PVRSRVReleaseSyncInfoKM() runs from the worker under the bridge lock.
 */
static void santos_sync_provider_release(void *token)
{
	struct santos_pvr_fence_syncinfo *psFenceSyncInfo = token;
	unsigned long flags;

	if (!psFenceSyncInfo)
		return;

	spin_lock_irqsave(&g_fenceSyncInfosLock, flags);
	psFenceSyncInfo->released = true;
	spin_unlock_irqrestore(&g_fenceSyncInfosLock, flags);

	santos_sync_provider_schedule_release();
}

/*
 * M5 completion callback.  Runs under the core's fence-list spinlock: read
 * the real SGX counters only, no locks, no sleeps.  The rule is the golden
 * PVRSyncHasSignaled() rule for the fence syncinfo.
 */
static bool santos_sync_provider_is_complete(void *token)
{
	struct santos_pvr_fence_syncinfo *psFenceSyncInfo = token;
	PVRSRV_SYNC_DATA *psSyncData;

	if (!psFenceSyncInfo || !psFenceSyncInfo->psBase)
		return false;

	/*
	 * Reset/abort semantics: a wrapper that the device reset caught
	 * incomplete must never report success afterwards, even if the
	 * counters change later.  The flag is written by the reset path under
	 * the provider list lock but read here without that lock (the core
	 * calls this under its own fence-list spinlock), hence READ_ONCE.
	 */
	if (READ_ONCE(psFenceSyncInfo->aborted))
		return false;

	psSyncData = psFenceSyncInfo->psBase->psSyncData;
	if (!psSyncData)
		return false;

	return psSyncData->ui32WriteOpsComplete >= psSyncData->ui32WriteOpsPending;
}

static const struct santos_sync_provider_ops g_santos_sync_provider_ops = {
	.is_complete = santos_sync_provider_is_complete,
	.release = santos_sync_provider_release,
	.owner = THIS_MODULE,
};

/*
 * Defer-free worker: process context, bridge lock held.  Moves every
 * released object that is no longer referenced by the GPU (or was aborted
 * by a reset/teardown) to a local list, then releases it outside the
 * spinlock.  Objects that are still in use stay queued and are retried on
 * the next M5 walk / release.
 */
static void santos_sync_provider_release_work(struct work_struct *work)
{
	struct santos_pvr_fence_syncinfo *psFenceSyncInfo, *psTmp;
	LIST_HEAD(sFreeList);
	unsigned long flags;
	bool bPending = false;

	PVR_UNREFERENCED_PARAMETER(work);

	LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);

	spin_lock_irqsave(&g_fenceSyncInfosLock, flags);
	list_for_each_entry_safe(psFenceSyncInfo, psTmp, &g_fenceSyncInfos, link)
	{
		if (!psFenceSyncInfo->released)
			continue;

		if (psFenceSyncInfo->aborted ||
		    !santos_sync_provider_syncinfo_in_use(psFenceSyncInfo->psBase))
			list_move_tail(&psFenceSyncInfo->link, &sFreeList);
		else
			bPending = true;
	}
	spin_unlock_irqrestore(&g_fenceSyncInfosLock, flags);

	list_for_each_entry_safe(psFenceSyncInfo, psTmp, &sFreeList, link)
	{
		list_del(&psFenceSyncInfo->link);
		PVRSRVReleaseSyncInfoKM(psFenceSyncInfo->psBase);
		psFenceSyncInfo->psBase = NULL;
		kfree(psFenceSyncInfo);
		module_put(THIS_MODULE);	/* provider object lifetime pin */
	}

	LinuxUnLockMutex(&gPVRSRVLock);

	atomic_set(&g_releasePending, bPending ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* services connection                                                 */
/* ------------------------------------------------------------------ */

/*
 * Services state used for syncinfo allocation is deliberately device-global:
 *
 *  - PVRSRVAcquireDeviceDataKM() resolves the SGX device node; no per-process
 *    state is created or retained.
 *  - PVRSRVAllocSyncInfoKM() uses the memory context only to reach the device
 *    node and the device-global sync heap; the vendor's own deviceclass code
 *    passes the kernel context for the same purpose (deviceclass.c:860).
 *
 * A process-owned device memory context must NOT be retained here: it is
 * registered under the creating process's resource-manager context
 * (BM_CreateContext -> ResManRegisterRes(RESMAN_TYPE_DEVICEMEM_CONTEXT)) and
 * is released when that process disconnects (resman.c:365), so a
 * module-lifetime global would dangle across compositor/workload restarts.
 *
 * Must be called with the bridge lock held (the first transfer path holds it).
 */
static int santos_sync_provider_connect_locked(void)
{
	PVRSRV_DEVICE_NODE *psDeviceNode;
	PVRSRV_ERROR eError;

	if (g_servicesConnected)
		return 0;

	eError = PVRSRVAcquireDeviceDataKM(0, PVRSRV_DEVICE_TYPE_SGX,
					   &g_servicesDevCookie);
	if (eError != PVRSRV_OK || !g_servicesDevCookie)
	{
		pr_warn(SANTOS_SYNC_PROVIDER_NAME ": PVRSRVAcquireDeviceDataKM failed (%d)\n", eError);
		g_servicesDevCookie = NULL;
		return -ENODEV;
	}

	psDeviceNode = (PVRSRV_DEVICE_NODE *)g_servicesDevCookie;
	g_servicesDevMemContext =
		(IMG_HANDLE)psDeviceNode->sDevMemoryInfo.pBMKernelContext;
	if (!g_servicesDevMemContext)
	{
		pr_warn(SANTOS_SYNC_PROVIDER_NAME ": no kernel device mem context\n");
		g_servicesDevCookie = NULL;
		return -ENODEV;
	}

	g_servicesConnected = true;
	return 0;
}

static void santos_sync_provider_disconnect(void)
{
	if (!g_servicesConnected)
		return;

	/*
	 * The kernel device memory context is owned by the device node for the
	 * module's lifetime; only drop the device lookup.
	 */
	g_servicesDevMemContext = NULL;
	g_servicesDevCookie = NULL;
	g_servicesConnected = false;
}

/* ------------------------------------------------------------------ */
/* core activation                                                     */
/* ------------------------------------------------------------------ */

static bool santos_sync_provider_get_symbols(void)
{
	if (g_symbolsHeld)
		return true;

	g_pfnSetProviderOps = symbol_get(santos_sync_set_provider_ops);
	g_pfnClearProviderOps = symbol_get(santos_sync_clear_provider_ops);
	g_pfnAllocBind = symbol_get(santos_sync_alloc_bind);
	g_pfnAllocClaim = symbol_get(santos_sync_alloc_claim);
	g_pfnAllocUnclaim = symbol_get(santos_sync_alloc_unclaim);
	g_pfnUpdateAll = symbol_get(santos_sync_update_all);

	if (g_pfnSetProviderOps && g_pfnClearProviderOps &&
	    g_pfnAllocBind && g_pfnAllocClaim && g_pfnAllocUnclaim &&
	    g_pfnUpdateAll)
	{
		g_symbolsHeld = true;
		return true;
	}

	if (g_pfnSetProviderOps)
		symbol_put(santos_sync_set_provider_ops);
	if (g_pfnClearProviderOps)
		symbol_put(santos_sync_clear_provider_ops);
	if (g_pfnAllocBind)
		symbol_put(santos_sync_alloc_bind);
	if (g_pfnAllocClaim)
		symbol_put(santos_sync_alloc_claim);
	if (g_pfnAllocUnclaim)
		symbol_put(santos_sync_alloc_unclaim);
	if (g_pfnUpdateAll)
		symbol_put(santos_sync_update_all);
	g_pfnSetProviderOps = NULL;
	g_pfnClearProviderOps = NULL;
	g_pfnAllocBind = NULL;
	g_pfnAllocClaim = NULL;
	g_pfnAllocUnclaim = NULL;
	g_pfnUpdateAll = NULL;
	return false;
}

static void santos_sync_provider_put_symbols(void)
{
	if (g_pfnSetProviderOps)
		symbol_put(santos_sync_set_provider_ops);
	if (g_pfnClearProviderOps)
		symbol_put(santos_sync_clear_provider_ops);
	if (g_pfnAllocBind)
		symbol_put(santos_sync_alloc_bind);
	if (g_pfnAllocClaim)
		symbol_put(santos_sync_alloc_claim);
	if (g_pfnAllocUnclaim)
		symbol_put(santos_sync_alloc_unclaim);
	if (g_pfnUpdateAll)
		symbol_put(santos_sync_update_all);
	g_pfnSetProviderOps = NULL;
	g_pfnClearProviderOps = NULL;
	g_pfnAllocBind = NULL;
	g_pfnAllocClaim = NULL;
	g_pfnAllocUnclaim = NULL;
	g_pfnUpdateAll = NULL;
	g_symbolsHeld = false;
}

/* Dormant when the core is absent: the stub keeps running unchanged. */
static void santos_sync_provider_try_activate(void)
{
	int ret;

	if (g_active || !g_releaseQueue)
		return;

	if (!santos_sync_provider_get_symbols())
	{
		pr_info(SANTOS_SYNC_PROVIDER_NAME ": santos sync core not present; legacy stub active\n");
		return;
	}

	ret = g_pfnSetProviderOps(&g_santos_sync_provider_ops);
	if (ret != 0)
	{
		pr_warn(SANTOS_SYNC_PROVIDER_NAME ": set_provider_ops failed (%d)\n", ret);
		santos_sync_provider_put_symbols();
		return;
	}

	g_active = true;
	pr_info(SANTOS_SYNC_PROVIDER_NAME ": M3 provider registered with santos sync core\n");
}

/* ------------------------------------------------------------------ */
/* alloc fd identification                                             */
/* ------------------------------------------------------------------ */

/*
 * A candidate is any file whose f_op owner is the core module.  The exact
 * alloc-vs-fence validation happens during the pre-submit claim: the core's
 * santos_sync_alloc_claim() rejects anything that is not one of its alloc
 * fds with -EINVAL before any provider object exists or any GPU command is
 * scheduled (a fence fd is therefore rejected pre-submit, not at commit).
 * The module pointer stays valid while we hold a reference to the file and
 * the core symbols (g_symbolsHeld).
 */
static bool santos_sync_provider_is_core_file(struct file *file)
{
	struct module *owner;

	if (!file || !file->f_op)
		return false;

	owner = file->f_op->owner;
	if (!owner)
		return false;

	return strcmp(owner->name, SANTOS_SYNC_CORE_MODULE) == 0;
}

/* ------------------------------------------------------------------ */
/* M3 transfer binding                                                 */
/* ------------------------------------------------------------------ */

/*
 * Roll the real syncinfo out of the CCB/completion world: the worker frees
 * it once the GPU no longer references it (or immediately when the write op
 * was rolled back / the device aborted).
 */
static void santos_sync_provider_defer_release(struct santos_pvr_fence_syncinfo *psFenceSyncInfo)
{
	unsigned long flags;

	if (!psFenceSyncInfo)
		return;

	spin_lock_irqsave(&g_fenceSyncInfosLock, flags);
	psFenceSyncInfo->released = true;
	spin_unlock_irqrestore(&g_fenceSyncInfosLock, flags);

	santos_sync_provider_schedule_release();
}

void PVRSyncProviderBindInit(struct santos_sync_transfer_bind *psBind)
{
	if (!psBind)
		return;

	psBind->file = NULL;
	psBind->syncinfo = NULL;
	psBind->psSyncInfo = NULL;
	psBind->claimed = IMG_FALSE;
}

/*
 * Prepare the real completion object for a transfer and place it into the
 * caller's device sync object.  The device sync snapshot is taken *before*
 * the write op is taken, matching the golden 3.4/1.12 pvr_sync.c and the
 * normal dst-sync path in sgxtransfer.c (the GPU/UKernel completes the
 * counters when the command finishes).
 *
 * Return values:
 *    0        object prepared; the caller must commit or abort
 *   -EINVAL   fd is not a core alloc file (legacy stub behaviour)
 *   -ENODEV   provider not active / core absent (legacy stub behaviour)
 *   -ENOMEM   real syncinfo allocation failed
 *   -EIO      services connection failed
 */
/*
 * M3 pre-submit reservation: fget the ALLOC fd, prove it is one of the
 * core's alloc fds (a fence fd of the same module owner is rejected here)
 * and claim it atomically before any provider object is allocated or any
 * GPU command is scheduled.
 *
 * Return values:
 *    0         claim held in psBind; commit() or abort() must follow
 *   -EINVAL    not a core alloc fd (legacy shell stub path)
 *   -EIO       core file, but the M3 provider cannot use it
 *   -EALREADY  alloc already consumed by CREATE
 *   -EBUSY     alloc already claimed/bound by another in-flight submit
 *   -EPERM     core file that is not an alloc fd (e.g. a fence fd)
 */
int PVRSyncProviderBindClaim(int iAllocFd, struct santos_sync_transfer_bind *psBind)
{
	struct file *file;
	int ret;

	if (!psBind)
		return -EINVAL;

	PVRSyncProviderBindInit(psBind);

	if (!g_active)
		santos_sync_provider_try_activate();

	file = fget(iAllocFd);
	if (!file)
		return -EINVAL;

	if (!santos_sync_provider_is_core_file(file))
	{
		/* Not one of the core's files: the shell stub keeps working. */
		fput(file);
		return -EINVAL;
	}

	if (!g_active || !g_pfnAllocClaim || !g_pfnAllocUnclaim)
	{
		/* A core file exists but the M3 provider cannot use it. */
		fput(file);
		return -EIO;
	}

	ret = g_pfnAllocClaim(file);
	if (ret != 0)
	{
		fput(file);
		if (ret == -EALREADY)
			return -EALREADY;	/* already consumed by CREATE */
		if (ret == -EBUSY)
			return -EBUSY;		/* another submit owns the alloc */
		return -EPERM;			/* core file, but not an alloc fd */
	}

	psBind->file = file;
	psBind->claimed = IMG_TRUE;
	return 0;
}

/*
 * Prepare the real completion object for a claimed alloc and place it into
 * the caller's device sync object.  Every fallible step (services
 * connection, allocations) happens before the provider write op is taken,
 * so a failed prepare never leaves a pending provider write op behind; the
 * failure path drops the claim itself via BindAbort().
 */
int PVRSyncProviderBindPrepare(struct santos_sync_transfer_bind *psBind,
			       PVRSRV_DEVICE_SYNC_OBJECT *psDevSync)
{
	struct santos_pvr_fence_syncinfo *psFenceSyncInfo;
	PVRSRV_KERNEL_SYNC_INFO *psSyncInfo = NULL;
	PVRSRV_SYNC_DATA *psSyncData;
	PVRSRV_ERROR eError;
	unsigned long flags;

	if (!psBind || !psDevSync || !psBind->file || !psBind->claimed ||
	    psBind->syncinfo)
		return -EINVAL;

	/*
	 * The transfer ioctl runs with the bridge lock held; the connection
	 * helper must not lock again.
	 */
	if (!g_servicesConnected && santos_sync_provider_connect_locked() != 0)
	{
		PVRSyncProviderBindAbort(psBind);
		return -EIO;
	}

	psFenceSyncInfo = kzalloc(sizeof(*psFenceSyncInfo), GFP_KERNEL);
	if (!psFenceSyncInfo)
	{
		PVRSyncProviderBindAbort(psBind);
		return -ENOMEM;
	}

	eError = PVRSRVAllocSyncInfoKM(g_servicesDevCookie,
				       g_servicesDevMemContext, &psSyncInfo);
	if (eError != PVRSRV_OK || !psSyncInfo)
	{
		kfree(psFenceSyncInfo);
		PVRSyncProviderBindAbort(psBind);
		return -ENOMEM;
	}

	/*
	 * Provider object lifetime pin.
	 *
	 * The core's per-token pin only protects the callback/token association
	 * until provider.release() is called.  The real wrapper and its
	 * syncinfo can still be referenced by SGX after that point (fence
	 * closed before completion, reset/abort reclamation, or a bind failure
	 * after a successful schedule where no token was ever accepted).  Pin
	 * this module from before the object can become visible to SGX until
	 * the wrapper is actually destroyed; the ordinary defer-free worker can
	 * always drop this pin, so it never has to wait for module_exit().
	 */
	if (!try_module_get(THIS_MODULE))
	{
		PVRSRVReleaseSyncInfoKM(psSyncInfo);
		kfree(psFenceSyncInfo);
		PVRSyncProviderBindAbort(psBind);
		return -ENODEV;
	}

	psFenceSyncInfo->psBase = psSyncInfo;

	spin_lock_irqsave(&g_fenceSyncInfosLock, flags);
	list_add_tail(&psFenceSyncInfo->link, &g_fenceSyncInfos);
	spin_unlock_irqrestore(&g_fenceSyncInfosLock, flags);

	/*
	 * Device sync snapshot first, then take the write op: exactly the
	 * ordering of the normal dst syncs built by SGXSubmitTransferKM.
	 * Nothing fallible follows this point.
	 */
	psSyncData = psSyncInfo->psSyncData;
	psDevSync->sReadOpsCompleteDevVAddr = psSyncInfo->sReadOpsCompleteDevVAddr;
	psDevSync->sWriteOpsCompleteDevVAddr = psSyncInfo->sWriteOpsCompleteDevVAddr;
	psDevSync->sReadOps2CompleteDevVAddr = psSyncInfo->sReadOps2CompleteDevVAddr;
	psDevSync->ui32WriteOpsPendingVal = psSyncData->ui32WriteOpsPending;
	psDevSync->ui32ReadOpsPendingVal = psSyncData->ui32ReadOpsPending;
	psDevSync->ui32ReadOps2PendingVal = psSyncData->ui32ReadOps2Pending;

	SyncTakeWriteOp(psSyncInfo, SYNC_OP_CLASS_TQ_3D);

	psBind->syncinfo = psFenceSyncInfo;	/* opaque token for the core */
	psBind->psSyncInfo = psSyncInfo;
	return 0;
}

/*
 * Commit the association to the core only after the SGX submit succeeded.
 * A failure here means the work *was* submitted (the syncinfo is in the
 * command) and userspace's CREATE will fail because the alloc stays
 * unbound; the object is still kept alive until the GPU is done.
 */
int PVRSyncProviderBindCommit(struct santos_sync_transfer_bind *psBind)
{
	int ret;

	if (!psBind || !psBind->file)
		return 0;

	/*
	 * The submit succeeded and the pre-submit claim reserved this alloc:
	 * alloc_bind() consumes the claim atomically.  With the claim held,
	 * -EBUSY/-EALREADY are unreachable; a failure here means the core is
	 * going away and the object must be kept alive until the GPU is done
	 * (the fence stays unbound and userspace's CREATE fails).
	 */
	ret = g_pfnAllocBind(psBind->file, psBind->syncinfo,
			     SANTOS_SYNC_BOUND_PENDING);
	if (ret != 0 && g_pfnAllocUnclaim)
		g_pfnAllocUnclaim(psBind->file);
	fput(psBind->file);
	psBind->file = NULL;
	psBind->claimed = IMG_FALSE;

	if (ret != 0)
	{
		pr_warn(SANTOS_SYNC_PROVIDER_NAME ": alloc_bind failed (%d) although the alloc was claimed\n", ret);
		santos_sync_provider_defer_release(psBind->syncinfo);
	}

	psBind->syncinfo = NULL;
	psBind->psSyncInfo = NULL;
	return ret;
}

/*
 * No token was accepted (transfer RETRY or a schedule failure): drop the
 * provider reference and let the worker free the object.  The caller has
 * already rolled the write op back, so it is normally freed immediately.
 */
void PVRSyncProviderBindAbort(struct santos_sync_transfer_bind *psBind)
{
	if (!psBind)
		return;

	if (psBind->file)
	{
		/* Drop an uncommitted claim so the same alloc can be retried. */
		if (psBind->claimed && g_pfnAllocUnclaim)
			g_pfnAllocUnclaim(psBind->file);
		fput(psBind->file);
		psBind->file = NULL;
	}
	psBind->claimed = IMG_FALSE;

	if (psBind->syncinfo)
	{
		santos_sync_provider_defer_release(psBind->syncinfo);
		psBind->syncinfo = NULL;
	}
	psBind->psSyncInfo = NULL;
}

/* ------------------------------------------------------------------ */
/* M5 and lifecycle                                                    */
/* ------------------------------------------------------------------ */

/*
 * Called from PVRSyncUpdateAllSyncs() in the MISR workqueue tail, after
 * PVRSRVMISR() processed the device completion (osfunc.c:1138) -- the same
 * authoritative point the golden driver uses.
 */
void PVRSyncProviderUpdateAllSyncs(void)
{
	if (g_active && g_pfnUpdateAll)
		(void)g_pfnUpdateAll();

	/*
	 * The walk may have completed the GPU work a released object was
	 * waiting for; retry the defer-free worker.
	 */
	if (atomic_read(&g_releasePending) && g_releaseQueue)
		queue_work(g_releaseQueue, &g_releaseWork);
}

/*
 * Device reset/recovery: in-flight work will never complete normally.  The
 * L11 ABI can only signal completion, so reset does not complete fences
 * (that would pretend failed work succeeded); it only allows the provider
 * to free objects whose tokens are already released.  Live fences stay
 * pending until userspace closes them.
 */
void PVRSyncProviderDeviceAbort(void)
{
	struct santos_pvr_fence_syncinfo *psFenceSyncInfo;
	unsigned long flags;

	spin_lock_irqsave(&g_fenceSyncInfosLock, flags);
	list_for_each_entry(psFenceSyncInfo, &g_fenceSyncInfos, link)
	{
		PVRSRV_SYNC_DATA *psSyncData;

		if (!psFenceSyncInfo->psBase)
			continue;
		psSyncData = psFenceSyncInfo->psBase->psSyncData;
		if (!psSyncData)
			continue;

		/*
		 * Preserve work that really completed before the reset; abort
		 * only work whose completion counters are still outstanding.
		 * Such an object can never be reported as successfully complete
		 * again (see is_complete()); a released object is still
		 * reclaimed by the defer-free worker.
		 */
		if (psSyncData->ui32WriteOpsComplete < psSyncData->ui32WriteOpsPending)
			WRITE_ONCE(psFenceSyncInfo->aborted, true);
	}
	spin_unlock_irqrestore(&g_fenceSyncInfosLock, flags);

	if (atomic_read(&g_releasePending))
		santos_sync_provider_schedule_release();
}

int PVRSyncProviderInit(void)
{
	g_releaseQueue = alloc_workqueue(SANTOS_SYNC_PROVIDER_NAME "-free",
					 WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!g_releaseQueue)
		return -ENOMEM;

	INIT_WORK(&g_releaseWork, santos_sync_provider_release_work);

	/*
	 * The core may already be loaded (normal gate order) or may be loaded
	 * later; the lazy attempt happens on the first candidate transfer.
	 */
	santos_sync_provider_try_activate();

	/*
	 * Services state is intentionally NOT acquired here.
	 *
	 * Nothing before the first transfer needs it, and doing it at module init
	 * runs in the insmod/loader process.  It is acquired lazily by
	 * PVRSyncProviderBindPrepare() on the first real transfer, and it is
	 * device-global (device node + kernel device memory context), so it is
	 * independent of which process submits (L24).
	 */

	return 0;
}

void PVRSyncProviderDeInit(void)
{
	g_active = false;

	if (g_symbolsHeld)
	{
		int ret = g_pfnClearProviderOps();

		if (ret != 0)
		{
			/*
			 * Cannot happen while this module is exiting: every
			 * accepted token holds a reference on this module, so
			 * module_exit() only runs with no outstanding tokens.
			 */
			pr_warn(SANTOS_SYNC_PROVIDER_NAME ": clear_provider_ops returned %d\n", ret);
		}
	}

	if (g_releaseQueue)
	{
		struct santos_pvr_fence_syncinfo *psFenceSyncInfo, *psTmp;

		cancel_work_sync(&g_releaseWork);

		/*
		 * Module teardown: the device side is going away with us, so
		 * every remaining object is aborted and released here.  A live
		 * token would have pinned this module; warn if the invariant
		 * was ever violated.
		 */
		spin_lock_irq(&g_fenceSyncInfosLock);
		list_for_each_entry(psFenceSyncInfo, &g_fenceSyncInfos, link)
		{
			if (!psFenceSyncInfo->released)
				pr_warn(SANTOS_SYNC_PROVIDER_NAME ": syncinfo still owned at exit\n");
			psFenceSyncInfo->aborted = true;
		}
		spin_unlock_irq(&g_fenceSyncInfosLock);

		LinuxLockMutexNested(&gPVRSRVLock, PVRSRV_LOCK_CLASS_BRIDGE);
		list_for_each_entry_safe(psFenceSyncInfo, psTmp,
					 &g_fenceSyncInfos, link)
		{
			list_del(&psFenceSyncInfo->link);
			PVRSRVReleaseSyncInfoKM(psFenceSyncInfo->psBase);
			psFenceSyncInfo->psBase = NULL;
			kfree(psFenceSyncInfo);
			module_put(THIS_MODULE);	/* object lifetime pin */
		}
		LinuxUnLockMutex(&gPVRSRVLock);

		atomic_set(&g_releasePending, 0);
		destroy_workqueue(g_releaseQueue);
		g_releaseQueue = NULL;
	}

	santos_sync_provider_disconnect();
	santos_sync_provider_put_symbols();
}
