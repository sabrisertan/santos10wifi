// SPDX-License-Identifier: GPL-2.0-only
/*
 * santos-sync-core.c - M1 real fence core for the Santos10 PVR native-sync
 * port (Linux 7.2, dma_fence + old Android '>' UAPI).
 *
 * Scope: standalone candidate module registering /dev/pvr_sync_core. It is
 * deliberately NOT linked into the live provider (santos_pvr112) or shell;
 * nothing loads it unless a maintainer does, so the validated always-signaled
 * compatibility stub keeps running in production. Integration seams are in
 * notes/l13-l11-implementation-20260914/L11-FENCE-CORE.md.
 *
 * ABI (1.12 vendor contract, L11-ABI-CONTRACT.md):
 *   PVR_SYNC_IOC_ALLOC_FENCE  ('W',2): reserve -> alloc fd (opaque token)
 *   PVR_SYNC_IOC_CREATE_FENCE ('W',0): alloc fd -> waitable fence fd
 *   PVR_SYNC_IOC_DEBUG_FENCE  ('W',1): -ENOTTY (provider types required)
 * Fence fds answer the OLD libsync UAPI on magic '>':
 *   WAIT(0) blocking wait (ms, <0 = forever), MERGE(1), FENCE_INFO(2),
 *   plus poll()/EPOLLIN.
 *
 * Ownership (reconciled with golden 1.12 pvr_sync.c):
 *   - ALLOC reserves an opaque syncinfo token. The transfer path binds it
 *     (sgxtransfer.c -> PVRSyncPatchTransferSyncInfos) BEFORE CREATE.
 *   - CREATE *consumes* the alloc once and moves the association to the
 *     fence (golden pvr_sync.c:747-751). CREATE is the LAST step of the
 *     vendor order: ALLOC -> transfer bind/submit -> CREATE -> wait.
 *   - The fence owns one dma_fence reference; the fence takes a module
 *     reference so it can outlive the module's own fds (escape to another
 *     consumer is safe until the fence is released).
 *   - Fence state is explicit and never inferred from a NULL syncinfo:
 *       BOUND_PENDING  real work bound (pending > 0 proven at CREATE)
 *       FAKE_COMPLETE  no work bound (pending == 0 proven at CREATE),
 *                      signaled immediately at CREATE (golden :799-800)
 *       UNBOUND        misuse/error; M5 never treats it as complete
 *   - M5 signaling entry point is santos_sync_update_all(tl, complete_cb):
 *     it signals only BOUND_PENDING fences whose syncinfo the callback
 *     proves complete. syncinfo == NULL is never completion.
 *   - .release frees the fence with dma_fence_free() (RCU-deferred kfree),
 *     never a direct kfree.
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/anon_inodes.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-unwrap.h>
#include <linux/sync_file.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/version.h>

#include "santos-sync-core.h"

#define SANTOS_SYNC_NAME	"santos-sync-core"
#define SANTOS_SYNC_MAX_NAME	32

/* Old Android sync UAPI (3.4 vendor variant, see L11-ABI-CONTRACT.md) */
#define SANTOS_SYNC_IOC_MAGIC	'>'

struct santos_sync_merge_data {
	__s32	fd2;
	char	name[SANTOS_SYNC_MAX_NAME];
	__s32	fence;
};

struct santos_sync_fence_info_data {
	__u32	len;
	char	name[SANTOS_SYNC_MAX_NAME];
	__s32	status;
};

#define SANTOS_SYNC_IOC_WAIT \
	_IOW(SANTOS_SYNC_IOC_MAGIC, 0, __s32)
#define SANTOS_SYNC_IOC_MERGE \
	_IOWR(SANTOS_SYNC_IOC_MAGIC, 1, struct santos_sync_merge_data)
#define SANTOS_SYNC_IOC_FENCE_INFO \
	_IOWR(SANTOS_SYNC_IOC_MAGIC, 2, struct santos_sync_fence_info_data)

/* PVR sync UAPI (vendor 1.12 pvr_sync_user.h) */
#define PVR_SYNC_IOC_MAGIC	'W'

struct pvr_sync_create_ioctl_data {
	char	name[SANTOS_SYNC_MAX_NAME];
	__s32	allocdSyncInfo;
	__s32	fence;
};

struct pvr_sync_alloc_ioctl_data {
	__s32	fence;
	__u32	bTimelineIdle;
};

#define PVR_SYNC_IOC_CREATE_FENCE \
	_IOWR(PVR_SYNC_IOC_MAGIC, 0, struct pvr_sync_create_ioctl_data)
#define PVR_SYNC_IOC_ALLOC_FENCE \
	_IOWR(PVR_SYNC_IOC_MAGIC, 2, struct pvr_sync_alloc_ioctl_data)

/*
 * PVR_SYNC_IOC_DEBUG_FENCE ('W',1) returns PVRSRV_SYNC_DATA metadata and
 * needs the provider's servicesext.h types; it is debug-only and is left
 * -ENOTTY here (same as today's stub).
 */

struct santos_sync_timeline {
	struct miscdevice	misc;
	u64			context;
	atomic64_t		seqno;
	struct list_head	fence_list;	/* live fences for M5 */
	spinlock_t		list_lock;
	/*
	 * Single lifetime invariant: incremented exactly once when a provider
	 * token enters the core (bind) and decremented exactly once after the
	 * final provider release() callback has returned. CREATE transfers the
	 * token without touching the count, so the token is continuously
	 * represented; clear_provider_ops() refuses while it is non-zero.
	 */
	atomic_t		outstanding_tokens;
	const struct santos_sync_provider_ops *provider_ops;
	struct mutex		ops_lock;	/* serializes bind/clear/snapshot */
	char			name[SANTOS_SYNC_MAX_NAME];
};

/* An alloc fd reserves an opaque provider association until CREATE. */
struct santos_sync_alloc {
	struct santos_sync_timeline	*timeline;
	spinlock_t			lock;		/* protects syncinfo/state/consumed/claimed */
	void				*syncinfo;	/* provider token */
	enum santos_sync_fence_state	state;
	bool				consumed;
	/*
	 * M3 pre-submit reservation: exactly one submit may prepare GPU work
	 * for this alloc between claim and bind.  A claim is consumed by a
	 * successful alloc_bind(), dropped by alloc_unclaim() (retry/hard
	 * failure) or by fd close.
	 */
	bool				claimed;
};

struct santos_sync_fence {
	struct dma_fence	base;		/* offset 0: freed with dma_fence_free */
	spinlock_t		lock;
	struct santos_sync_timeline *timeline;
	struct list_head	link;
	void			*syncinfo;	/* valid for bound fences */
	enum santos_sync_fence_state state;
	char			name[SANTOS_SYNC_MAX_NAME];
};

static const struct file_operations santos_sync_fence_fops;

#define SANTOS_FENCE_POLL_ENABLED	0

struct santos_sync_fence_file {
	struct dma_fence	*fence;
	struct wait_queue_head	wq;
	struct dma_fence_cb	cb;
	unsigned long		flags;
	char			name[SANTOS_SYNC_MAX_NAME];
};

/* ------------------------------------------------------------------ */
/* fence objects                                                       */
/* ------------------------------------------------------------------ */

/* Singleton timeline registered at module init. */
static struct santos_sync_timeline *santos_sync_timeline;

static const char *santos_sync_get_driver_name(struct dma_fence *fence)
{
	return SANTOS_SYNC_NAME;
}

static const char *santos_sync_get_timeline_name(struct dma_fence *fence)
{
	return SANTOS_SYNC_NAME "-timeline";
}

/*
 * Final owner-side release of a provider token.
 *
 * Ordering (the token's own module reference pins the provider throughout):
 *   1. provider release() runs and returns;
 *   2. the token stops being outstanding (atomic_dec) - at the instant the
 *      count can reach zero the provider module is still pinned;
 *   3. only then is the token's provider module reference dropped.
 * A concurrent clear_provider_ops() therefore either observes a non-zero
 * count (and refuses) or runs after step 2 while the module is still
 * resident, so it can clear the raw ops pointer before module_exit() is
 * allowed to complete. The local ops snapshot is used only while its module
 * reference is still held; tl->provider_ops is never re-read afterwards.
 * release() is a non-sleeping callback by contract.
 */
static void santos_sync_token_release(struct santos_sync_timeline *tl,
				      void *syncinfo)
{
	const struct santos_sync_provider_ops *ops;

	if (!syncinfo)
		return;

	ops = READ_ONCE(tl->provider_ops);
	if (ops && ops->release)
		ops->release(syncinfo);
	else
		WARN_ON_ONCE(1);	/* contract violation: token without ops */

	/* Count first: when it reaches zero the module must still be pinned. */
	atomic_dec(&tl->outstanding_tokens);

	if (ops && ops->owner)
		module_put(ops->owner);
}

/*
 * Final fence release. dma_fence_free() performs an RCU-deferred kfree; a
 * direct kfree here would be a use-after-free for RCU-protected readers.
 * The per-fence module reference is dropped last: any live fence (including
 * one that escaped to another consumer) keeps this module resident.
 */
static void santos_sync_fence_free(struct dma_fence *fence)
{
	struct santos_sync_fence *sf =
		container_of(fence, struct santos_sync_fence, base);
	unsigned long flags;
	void *syncinfo = sf->syncinfo;

	spin_lock_irqsave(&sf->timeline->list_lock, flags);
	list_del(&sf->link);
	spin_unlock_irqrestore(&sf->timeline->list_lock, flags);

	/* The token stays counted until the callback has returned. */
	santos_sync_token_release(sf->timeline, syncinfo);

	dma_fence_free(fence);
	module_put(THIS_MODULE);
}

static const struct dma_fence_ops santos_sync_fence_ops = {
	.get_driver_name	= santos_sync_get_driver_name,
	.get_timeline_name	= santos_sync_get_timeline_name,
	.release		= santos_sync_fence_free,
};

static struct dma_fence *
santos_sync_fence_create(struct santos_sync_timeline *tl, const char *name,
			 enum santos_sync_fence_state state, void *syncinfo)
{
	struct santos_sync_fence *sf;
	unsigned long flags;
	u64 seqno;

	if (!try_module_get(THIS_MODULE))
		return NULL;

	sf = kzalloc(sizeof(*sf), GFP_KERNEL);
	if (!sf) {
		module_put(THIS_MODULE);
		return NULL;
	}

	spin_lock_init(&sf->lock);
	sf->timeline = tl;
	sf->syncinfo = syncinfo;
	sf->state = state;
	strscpy(sf->name, name && name[0] ? name : SANTOS_SYNC_NAME,
		sizeof(sf->name));

	seqno = atomic64_inc_return(&tl->seqno);
	dma_fence_init(&sf->base, &santos_sync_fence_ops, &sf->lock,
		       tl->context, seqno);

	spin_lock_irqsave(&tl->list_lock, flags);
	list_add_tail(&sf->link, &tl->fence_list);
	spin_unlock_irqrestore(&tl->list_lock, flags);

	return &sf->base;
}

/*
 * M5 entry point: signal exactly once. Callers must have proven completion;
 * this module never signals from submission.
 */
void santos_sync_signal_fence(struct dma_fence *fence)
{
	if (!fence)
		return;
	if (dma_fence_is_signaled(fence))
		return;
	dma_fence_signal(fence);
}
EXPORT_SYMBOL_GPL(santos_sync_signal_fence);

bool santos_sync_fence_is_signaled(struct dma_fence *fence)
{
	return fence && dma_fence_is_signaled(fence);
}
EXPORT_SYMBOL_GPL(santos_sync_fence_is_signaled);

/*
 * M5 walk (zero-context API for the provider MISR tail).
 *
 * Signals every BOUND_PENDING fence whose syncinfo the provider proves
 * complete. The completed fence is *claimed under list_lock* by moving it to
 * SANTOS_SYNC_BOUND_SIGNALED before the lock is dropped, so two concurrent
 * walkers can never select the same object twice and a walk always makes
 * forward progress. UNBOUND fences (syncinfo == NULL) are never completion;
 * FAKE_COMPLETE fences were already signaled at CREATE.
 *
 * provider_ops->is_complete() runs under the spinlock and must not sleep.
 */
int santos_sync_update_all(void)
{
	struct santos_sync_timeline *tl = santos_sync_timeline;
	const struct santos_sync_provider_ops *ops;
	struct santos_sync_fence *sf;
	unsigned long flags;

	if (!tl)
		return -ENODEV;

	/*
	 * Snapshot the provider callbacks and pin the provider module for the
	 * duration of the walk. ops_lock is a mutex, so this function must be
	 * called from sleepable context (the MISR workqueue tail); the
	 * is_complete() callback itself still runs under the fence-list
	 * spinlock and must not sleep.
	 */
	mutex_lock(&tl->ops_lock);
	ops = READ_ONCE(tl->provider_ops);
	if (ops && ops->owner && !try_module_get(ops->owner))
		ops = NULL;		/* provider is going away */
	mutex_unlock(&tl->ops_lock);
	if (!ops || !ops->is_complete)
		return -ENOSYS;

restart:
	spin_lock_irqsave(&tl->list_lock, flags);
	list_for_each_entry(sf, &tl->fence_list, link) {
		if (sf->state != SANTOS_SYNC_BOUND_PENDING || !sf->syncinfo)
			continue;	/* unbound/claimed are never completion */
		if (!ops->is_complete(sf->syncinfo))
			continue;
		/*
		 * The final dma_fence_put() can drop the last reference while
		 * the release path is still waiting for list_lock to unlink this
		 * fence (the object is removed from the list before it is
		 * freed). Acquire a reference only if the fence is still alive;
		 * list_lock keeps the storage valid for the lookup, and a fence
		 * that is already at zero is skipped - its release path will
		 * unlink it after we drop the lock.
		 */
		if (!dma_fence_get_rcu(&sf->base))
			continue;
		/* Claim before dropping the lock: this is the progress rule. */
		sf->state = SANTOS_SYNC_BOUND_SIGNALED;
		spin_unlock_irqrestore(&tl->list_lock, flags);
		santos_sync_signal_fence(&sf->base);
		dma_fence_put(&sf->base);
		goto restart;	/* the claimed fence is skipped now */
	}
	spin_unlock_irqrestore(&tl->list_lock, flags);
	if (ops->owner)
		module_put(ops->owner);
	return 0;
}
EXPORT_SYMBOL_GPL(santos_sync_update_all);

int santos_sync_set_provider_ops(const struct santos_sync_provider_ops *ops)
{
	struct santos_sync_timeline *tl = santos_sync_timeline;
	int ret = 0;

	if (!tl)
		return -ENODEV;
	if (!ops || !ops->is_complete || !ops->release)
		return -EINVAL;

	mutex_lock(&tl->ops_lock);
	if (tl->provider_ops || atomic_read(&tl->outstanding_tokens))
		ret = -EBUSY;
	else
		WRITE_ONCE(tl->provider_ops, ops);
	mutex_unlock(&tl->ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(santos_sync_set_provider_ops);

int santos_sync_clear_provider_ops(void)
{
	struct santos_sync_timeline *tl = santos_sync_timeline;
	int ret = 0;

	if (!tl)
		return -ENODEV;

	/*
	 * The outstanding_tokens invariant is the only guard: it is non-zero
	 * for every token that has entered the core (alloc or fence) and drops
	 * only after the final release() callback returned. bind() admits tokens
	 * under the same ops_lock, so once this check passes no new token can
	 * appear and no callback can be in flight. A fence-list scan is not used
	 * as the lifetime guarantee.
	 */
	mutex_lock(&tl->ops_lock);
	if (atomic_read(&tl->outstanding_tokens))
		ret = -EBUSY;
	else
		WRITE_ONCE(tl->provider_ops, NULL);
	mutex_unlock(&tl->ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(santos_sync_clear_provider_ops);

/* ------------------------------------------------------------------ */
/* old-UAPI fence file                                                 */
/* ------------------------------------------------------------------ */

static void santos_sync_fence_cb(struct dma_fence *fence,
				 struct dma_fence_cb *cb)
{
	struct santos_sync_fence_file *ff =
		container_of(cb, struct santos_sync_fence_file, cb);
	wake_up_all(&ff->wq);
}

static __poll_t santos_sync_fence_poll(struct file *file, poll_table *wait)
{
	struct santos_sync_fence_file *ff = file->private_data;

	poll_wait(file, &ff->wq, wait);
	/*
	 * Concurrent pollers must not double-register the single callback:
	 * test_and_set_bit serialises registration exactly like sync_file.c.
	 * A racing release is impossible while poll runs because the syscall
	 * holds the file reference; remove_callback handles an in-flight
	 * callback invocation.
	 */
	if (!test_and_set_bit(SANTOS_FENCE_POLL_ENABLED, &ff->flags)) {
		if (dma_fence_add_callback(ff->fence, &ff->cb,
					   santos_sync_fence_cb) < 0)
			wake_up_all(&ff->wq);
	}
	return dma_fence_is_signaled(ff->fence) ? EPOLLIN : 0;
}

/* old-UAPI WAIT: ms timeout, <0 waits indefinitely. */
static long santos_sync_fence_ioctl_wait(struct santos_sync_fence_file *ff,
					 unsigned long arg)
{
	__s32 timeout_ms;
	signed long timeout;
	signed long ret;

	if (copy_from_user(&timeout_ms, (void __user *)arg, sizeof(timeout_ms)))
		return -EFAULT;

	timeout = timeout_ms < 0 ? MAX_SCHEDULE_TIMEOUT :
				  msecs_to_jiffies(timeout_ms);
	ret = dma_fence_wait_timeout(ff->fence, true, timeout);
	if (ret > 0)
		return 0;
	if (ret == 0)
		return -ETIME;
	return ret;
}

/* Resolve any userspace fence fd to a dma_fence reference. */
static struct dma_fence *santos_sync_fence_from_fd(int fd)
{
	struct file *file;
	struct dma_fence *fence = NULL;

	file = fget(fd);
	if (!file)
		return ERR_PTR(-ENOENT);

	if (file->f_op == &santos_sync_fence_fops) {
		struct santos_sync_fence_file *ff = file->private_data;
		fence = dma_fence_get(ff->fence);
	} else {
		fence = sync_file_get_fence(fd);	/* modern sync_file */
	}
	fput(file);
	return fence ? fence : ERR_PTR(-ENOENT);
}

static long santos_sync_fence_ioctl_merge(struct santos_sync_fence_file *ff,
					  unsigned long arg)
{
	struct santos_sync_merge_data data;
	struct dma_fence *fence2, *merged;
	struct santos_sync_fence_file *nf;
	struct file *newfile;
	int fd, ret = 0;

	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;

	if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
		ret = -EFAULT;
		goto err_fd;
	}
	data.name[sizeof(data.name) - 1] = '\0';

	fence2 = santos_sync_fence_from_fd(data.fd2);
	if (IS_ERR(fence2)) {
		ret = PTR_ERR(fence2);
		goto err_fd;
	}

	/*
	 * dma_fence_unwrap_merge() returns a fence with its own reference and
	 * does not consume the inputs; fence2 is ours to put afterwards.
	 */
	merged = dma_fence_unwrap_merge(ff->fence, fence2);
	dma_fence_put(fence2);
	if (!merged) {
		ret = -ENOMEM;
		goto err_fd;
	}

	nf = kzalloc(sizeof(*nf), GFP_KERNEL);
	if (!nf) {
		dma_fence_put(merged);
		ret = -ENOMEM;
		goto err_fd;
	}
	nf->fence = merged;
	init_waitqueue_head(&nf->wq);
	strscpy(nf->name, data.name, sizeof(nf->name));

	newfile = anon_inode_getfile(SANTOS_SYNC_NAME "-fence",
				     &santos_sync_fence_fops, nf, O_RDWR);
	if (IS_ERR(newfile)) {
		dma_fence_put(merged);
		kfree(nf);
		ret = PTR_ERR(newfile);
		goto err_fd;
	}

	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		fput(newfile);
		ret = -EFAULT;
		goto err_fd;
	}
	fd_install(fd, newfile);
	return 0;

err_fd:
	put_unused_fd(fd);
	return ret;
}

static long santos_sync_fence_ioctl_info(struct santos_sync_fence_file *ff,
					 unsigned long arg)
{
	struct santos_sync_fence_info_data info;
	__u32 user_len;
	int status;

	if (get_user(user_len, (__u32 __user *)arg))
		return -EFAULT;
	if (user_len < sizeof(info))
		return -EINVAL;

	memset(&info, 0, sizeof(info));
	info.len = sizeof(info);
	strscpy(info.name, ff->name, sizeof(info.name));
	status = dma_fence_get_status(ff->fence);
	info.status = status < 0 ? status : (status > 0 ? 1 : 0);

	if (copy_to_user((void __user *)arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static long santos_sync_fence_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	struct santos_sync_fence_file *ff = file->private_data;

	switch (cmd) {
	case SANTOS_SYNC_IOC_WAIT:
		return santos_sync_fence_ioctl_wait(ff, arg);
	case SANTOS_SYNC_IOC_MERGE:
		return santos_sync_fence_ioctl_merge(ff, arg);
	case SANTOS_SYNC_IOC_FENCE_INFO:
		return santos_sync_fence_ioctl_info(ff, arg);
	default:
		return -ENOTTY;
	}
}

static int santos_sync_fence_release(struct inode *inode, struct file *file)
{
	struct santos_sync_fence_file *ff = file->private_data;

	if (test_bit(SANTOS_FENCE_POLL_ENABLED, &ff->flags))
		dma_fence_remove_callback(ff->fence, &ff->cb);
	dma_fence_put(ff->fence);
	kfree(ff);
	return 0;
}

static const struct file_operations santos_sync_fence_fops = {
	.owner		= THIS_MODULE,
	.release	= santos_sync_fence_release,
	.poll		= santos_sync_fence_poll,
	.unlocked_ioctl	= santos_sync_fence_ioctl,
	.compat_ioctl	= santos_sync_fence_ioctl,
	.llseek		= noop_llseek,
};

/* ------------------------------------------------------------------ */
/* alloc fd                                                            */
/* ------------------------------------------------------------------ */

static int santos_sync_alloc_release(struct inode *inode, struct file *file)
{
	struct santos_sync_alloc *alloc = file->private_data;
	struct santos_sync_timeline *tl = alloc->timeline;
	void *syncinfo;

	/*
	 * Take the association under the per-alloc lock: CREATE either ran
	 * first (it owns the token; this close must not release it) or this
	 * close runs first (the alloc owns the token and releases it exactly
	 * once; a following CREATE observes the UNBOUND state and fails).
	 */
	spin_lock(&alloc->lock);
	syncinfo = alloc->syncinfo;
	alloc->syncinfo = NULL;	/* clear before release: exactly once */
	alloc->state = SANTOS_SYNC_UNBOUND;
	spin_unlock(&alloc->lock);

	/*
	 * release() is a non-sleeping callback by contract, so it is safe in
	 * the file-release path. clear_provider_ops() refuses while any
	 * outstanding token exists, so the ops cannot have been cleared here.
	 */
	if (syncinfo)
		santos_sync_token_release(tl, syncinfo);
	kfree(alloc);
	return 0;
}

static const struct file_operations santos_sync_alloc_fops = {
	.owner		= THIS_MODULE,
	.release	= santos_sync_alloc_release,
	.llseek		= noop_llseek,
};

/*
 * M3 pre-submit reservation (internal API, no userspace UAPI).
 *
 * The provider fget()s the ALLOC fd and claims it before allocating the
 * fence syncinfo or scheduling any GPU command.  The claim gives the
 * prepare -> schedule -> commit sequence atomicity against concurrent
 * submitters, and it performs the exact alloc-fops validation up front:
 * anything that is not one of this module's alloc fds (for example one of
 * the fence fds, which shares the module owner) is rejected before any
 * provider object or GPU command exists.
 *
 *   -EALREADY  the alloc was already consumed by CREATE
 *   -EBUSY     another submit already claimed it (or it is already bound)
 *    0         claim held by the caller; release with unclaim() or consume
 *              it with a successful alloc_bind()
 */
int santos_sync_alloc_claim(struct file *file)
{
	struct santos_sync_alloc *alloc;
	int ret = 0;

	if (!file || file->f_op != &santos_sync_alloc_fops)
		return -EINVAL;

	alloc = file->private_data;

	spin_lock(&alloc->lock);
	if (alloc->consumed)
		ret = -EALREADY;
	else if (alloc->claimed || alloc->syncinfo ||
		 alloc->state != SANTOS_SYNC_UNBOUND)
		ret = -EBUSY;
	else
		alloc->claimed = true;
	spin_unlock(&alloc->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(santos_sync_alloc_claim);

/*
 * Drop an uncommitted claim (transfer RETRY / hard pre-submit failure).
 * The alloc stays fully usable: it can be claimed and bound again.
 */
void santos_sync_alloc_unclaim(struct file *file)
{
	struct santos_sync_alloc *alloc;

	if (!file || file->f_op != &santos_sync_alloc_fops)
		return;

	alloc = file->private_data;

	spin_lock(&alloc->lock);
	alloc->claimed = false;
	spin_unlock(&alloc->lock);
}
EXPORT_SYMBOL_GPL(santos_sync_alloc_unclaim);

/*
 * Provider binding helper (M3): associate the alloc's opaque syncinfo with
 * an explicit state before CREATE. Must be called before CREATE consumes
 * the alloc; a second bind or a bind after CREATE fails.  A BOUND_PENDING
 * bind requires the alloc's pre-submit claim (core-enforced -EPERM
 * otherwise) and consumes it atomically: with the claim held this is the
 * commit point, not an expected -EBUSY path.  A FAKE_COMPLETE binding
 * carries no work and does not require a claim.
 */
int santos_sync_alloc_bind(struct file *file, void *syncinfo, int state)
{
	struct santos_sync_timeline *tl = santos_sync_timeline;
	struct santos_sync_alloc *alloc;
	const struct santos_sync_provider_ops *ops;
	int ret = 0;

	if (!file || file->f_op != &santos_sync_alloc_fops)
		return -EINVAL;
	if (!tl)
		return -ENODEV;

	alloc = file->private_data;

	/*
	 * Explicit ownership cases:
	 *   UNBOUND       : nothing to bind (rejected);
	 *   BOUND_PENDING : a completion token is required;
	 *   FAKE_COMPLETE : proven no-work fence; a token is optional and is
	 *                   released at fence teardown if present.
	 */
	if (state == SANTOS_SYNC_BOUND_PENDING && !syncinfo)
		return -EINVAL;
	if (state != SANTOS_SYNC_BOUND_PENDING &&
	    state != SANTOS_SYNC_FAKE_COMPLETE)
		return -EINVAL;

	/*
	 * Admission is serialized against clear_provider_ops() by ops_lock:
	 * the ops pointer is observed and the provider module reference is
	 * taken while the lock is held, so a concurrent clear can never let a
	 * token exist without valid callbacks. try_module_get() failing means
	 * the provider is going away; the token is rejected before it becomes
	 * visible.
	 */
	mutex_lock(&tl->ops_lock);
	ops = READ_ONCE(tl->provider_ops);
	if (!ops || !ops->is_complete || !ops->release) {
		ret = -ENOSYS;
		goto out_unlock;
	}
	if (syncinfo && ops->owner && !try_module_get(ops->owner)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	/*
	 * The per-alloc lock is the ownership gate: the single-bind check and
	 * the state/syncinfo transition are atomic against CREATE and against
	 * fd close. If another bind or CREATE won the race, undo the admission
	 * (module reference; the counter is bumped only on success) and fail
	 * deterministically.
	 */
	spin_lock(&alloc->lock);
	if (alloc->consumed || alloc->syncinfo ||
	    alloc->state != SANTOS_SYNC_UNBOUND) {
		bool consumed = alloc->consumed;

		spin_unlock(&alloc->lock);
		if (syncinfo && ops->owner)
			module_put(ops->owner);
		ret = consumed ? -EALREADY : -EBUSY;
		goto out_unlock;
	}

	/*
	 * M3 commit invariant: a real (BOUND_PENDING) association carries GPU
	 * work and may only be created by the alloc's pre-submit claim.  This
	 * makes "successful schedule -> bind commit" a core-enforced property
	 * instead of a provider convention.  A no-work FAKE_COMPLETE binding
	 * does not require a claim.
	 */
	if (state == SANTOS_SYNC_BOUND_PENDING && !alloc->claimed) {
		spin_unlock(&alloc->lock);
		if (syncinfo && ops->owner)
			module_put(ops->owner);
		ret = -EPERM;
		goto out_unlock;
	}

	alloc->syncinfo = syncinfo;
	alloc->state = state;
	alloc->claimed = false;	/* commit consumes the M3 reservation */
	if (syncinfo)
		atomic_inc(&tl->outstanding_tokens);
	spin_unlock(&alloc->lock);
out_unlock:
	mutex_unlock(&tl->ops_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(santos_sync_alloc_bind);

/* ------------------------------------------------------------------ */
/* /dev/pvr_sync_core                                                  */
/* ------------------------------------------------------------------ */

static long santos_pvr_sync_ioctl(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	struct santos_sync_timeline *tl = file->private_data;

	switch (cmd) {
	case PVR_SYNC_IOC_ALLOC_FENCE: {
		struct pvr_sync_alloc_ioctl_data data;
		struct santos_sync_alloc *alloc;
		struct file *allocfile;
		int fd;

		alloc = kzalloc(sizeof(*alloc), GFP_KERNEL);
		if (!alloc)
			return -ENOMEM;
		alloc->timeline = tl;
		alloc->state = SANTOS_SYNC_UNBOUND;
		spin_lock_init(&alloc->lock);

		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			kfree(alloc);
			return fd;
		}
		allocfile = anon_inode_getfile(SANTOS_SYNC_NAME "-alloc",
					       &santos_sync_alloc_fops,
					       alloc, O_RDWR);
		if (IS_ERR(allocfile)) {
			kfree(alloc);
			put_unused_fd(fd);
			return PTR_ERR(allocfile);
		}
		data.fence = fd;
		data.bTimelineIdle = 1;
		if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
			fput(allocfile);
			put_unused_fd(fd);
			return -EFAULT;
		}
		fd_install(fd, allocfile);
		return 0;
	}
	case PVR_SYNC_IOC_CREATE_FENCE: {
		struct pvr_sync_create_ioctl_data data;
		struct santos_sync_alloc *alloc;
		struct santos_sync_fence_file *nf;
		struct dma_fence *fence = NULL;
		struct file *allocfile, *newfile;
		void *syncinfo;
		enum santos_sync_fence_state state;
		int fd, ret = 0;

		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0)
			return fd;

		if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
			ret = -EFAULT;
			goto err_fd;
		}
		data.name[sizeof(data.name) - 1] = '\0';

		allocfile = fget(data.allocdSyncInfo);
		if (!allocfile) {
			ret = -ENOENT;
			goto err_fd;
		}
		if (allocfile->f_op != &santos_sync_alloc_fops) {
			fput(allocfile);
			ret = -EINVAL;
			goto err_fd;
		}
		alloc = allocfile->private_data;

		/*
		 * Consume the alloc under its own lock: the check and the
		 * state/syncinfo transition are atomic, so two concurrent
		 * CREATE calls can never both copy the same token.
		 *
		 * The documented order is ALLOC -> bind/submit -> CREATE. An
		 * UNBOUND alloc was never associated with GPU work; consuming it
		 * here would manufacture a permanently-unsignaled fence. Reject
		 * before consuming so the alloc stays usable after a bind.  This
		 * is also the deterministic answer to CREATE racing an M3 claim:
		 * the claim does not change the state, so CREATE fails with
		 * -EINVAL without consuming and the in-flight submit can still
		 * commit.
		 */
		spin_lock(&alloc->lock);
		if (alloc->consumed) {
			spin_unlock(&alloc->lock);
			fput(allocfile);
			ret = -EALREADY;
			goto err_fd;
		}
		if (alloc->state == SANTOS_SYNC_UNBOUND) {
			spin_unlock(&alloc->lock);
			fput(allocfile);
			ret = -EINVAL;
			goto err_fd;
		}
		alloc->consumed = true;
		syncinfo = alloc->syncinfo;
		state = alloc->state;
		alloc->syncinfo = NULL;	/* golden move: association leaves the alloc */
		spin_unlock(&alloc->lock);
		fput(allocfile);

		/*
		 * The token stays counted in outstanding_tokens across the move,
		 * so clear_provider_ops() cannot clear the provider between the
		 * alloc and the fence. Failure paths below must release the token
		 * exactly once: before fence creation directly, afterwards through
		 * the fence's final dma_fence_put().
		 */
		fence = santos_sync_fence_create(tl, data.name, state, syncinfo);
		if (!fence) {
			santos_sync_token_release(tl, syncinfo);
			ret = -ENOMEM;
			goto err_fd;
		}

		nf = kzalloc(sizeof(*nf), GFP_KERNEL);
		if (!nf) {
			dma_fence_put(fence);
			ret = -ENOMEM;
			goto err_fd;
		}
		nf->fence = fence;
		init_waitqueue_head(&nf->wq);
		strscpy(nf->name, data.name, sizeof(nf->name));

		newfile = anon_inode_getfile(SANTOS_SYNC_NAME "-fence",
					     &santos_sync_fence_fops, nf,
					     O_RDWR);
		if (IS_ERR(newfile)) {
			dma_fence_put(fence);
			kfree(nf);
			ret = PTR_ERR(newfile);
			goto err_fd;
		}
		data.fence = fd;
		if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
			fput(newfile);
			ret = -EFAULT;
			goto err_fd;
		}
		fd_install(fd, newfile);

		/* Proven no-work fence: signal explicitly at CREATE (golden). */
		if (state == SANTOS_SYNC_FAKE_COMPLETE)
			santos_sync_signal_fence(fence);
		return 0;

err_fd:
		put_unused_fd(fd);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}

static int santos_pvr_sync_open(struct inode *inode, struct file *file)
{
	if (!santos_sync_timeline)
		return -ENODEV;
	file->private_data = santos_sync_timeline;
	return 0;
}

static int santos_pvr_sync_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations santos_pvr_sync_fops = {
	.owner		= THIS_MODULE,
	.open		= santos_pvr_sync_open,
	.release	= santos_pvr_sync_release,
	.unlocked_ioctl	= santos_pvr_sync_ioctl,
	.compat_ioctl	= santos_pvr_sync_ioctl,
	.llseek		= noop_llseek,
};

/* ------------------------------------------------------------------ */
/* module                                                              */
/* ------------------------------------------------------------------ */

static int __init santos_sync_core_init(void)
{
	struct santos_sync_timeline *tl;
	int ret;

	tl = kzalloc(sizeof(*tl), GFP_KERNEL);
	if (!tl)
		return -ENOMEM;

	tl->context = dma_fence_context_alloc(1);
	atomic64_set(&tl->seqno, 0);
	INIT_LIST_HEAD(&tl->fence_list);
	spin_lock_init(&tl->list_lock);
	mutex_init(&tl->ops_lock);
	atomic_set(&tl->outstanding_tokens, 0);
	tl->provider_ops = NULL;
	strscpy(tl->name, "pvr_sync_core", sizeof(tl->name));

	tl->misc.minor = MISC_DYNAMIC_MINOR;
	tl->misc.name = "pvr_sync_core";
	tl->misc.fops = &santos_pvr_sync_fops;
	tl->misc.mode = 0666;

	santos_sync_timeline = tl;
	ret = misc_register(&tl->misc);
	if (ret) {
		santos_sync_timeline = NULL;
		kfree(tl);
		return ret;
	}
	pr_info(SANTOS_SYNC_NAME ": core registered (standalone candidate)\n");
	return 0;
}

static void __exit santos_sync_core_exit(void)
{
	if (!santos_sync_timeline)
		return;
	misc_deregister(&santos_sync_timeline->misc);
	kfree(santos_sync_timeline);
	santos_sync_timeline = NULL;
	pr_info(SANTOS_SYNC_NAME ": core unregistered\n");
}

module_init(santos_sync_core_init);
module_exit(santos_sync_core_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("Standalone real fence core (M1) for the Santos10 PVR sync port");
MODULE_LICENSE("GPL");
