/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * santos-sync-core.h - provider-facing API of the Santos10 M1 fence core.
 *
 * The core owns the timeline and the fence objects; the provider (M3
 * integration in the PVR driver) owns the opaque completion association
 * (syncinfo token) and supplies the two non-sleeping callbacks below.
 *
 * Ownership contract (see L11-ABI-CONTRACT.md):
 *   - set_provider_ops() before any alloc_bind(); clear_provider_ops()
 *     refuses (-EBUSY) while a bound alloc or fence still owns a token;
 *   - alloc_bind() transfers the provider token association into the core;
 *     UNBOUND bindings are rejected, BOUND_PENDING requires a token,
 *     FAKE_COMPLETE may carry an optional token;
 *   - CREATE moves the token from the alloc fd to the fence;
 *   - release() is called exactly once: from alloc-fd close when CREATE
 *     never consumed the binding, otherwise from the final fence release;
 *   - is_complete() is called only while the fence is BOUND_PENDING and
 *     must not sleep; release() must not sleep either (a provider that
 *     needs a mutex defers the work, golden gSyncInfoFreeList pattern);
 *   - owner is the provider module; the core takes one module reference per
 *     accepted token. On the final release path the token's outstanding
 *     count is dropped first and the module reference last, so the module is
 *     still pinned when the count reaches zero and clear_provider_ops() (the
 *     provider's module_exit step) can clear the raw ops pointer before the
 *     module storage disappears. try_module_get() failure rejects the token
 *     cleanly during teardown.
 */
#ifndef _SANTOS_SYNC_CORE_H
#define _SANTOS_SYNC_CORE_H

#include <linux/types.h>
#include <linux/dma-fence.h>

enum santos_sync_fence_state {
	SANTOS_SYNC_UNBOUND = 0,
	SANTOS_SYNC_BOUND_PENDING,
	SANTOS_SYNC_BOUND_SIGNALED,	/* core-internal post-claim state */
	SANTOS_SYNC_FAKE_COMPLETE,
};

struct module;

struct santos_sync_provider_ops {
	bool (*is_complete)(void *syncinfo);
	void (*release)(void *syncinfo);
	struct module *owner;	/* provider module; NULL for built-in providers */
};

struct file;

/* M5 completion entry points (called from the provider MISR workqueue). */
void santos_sync_signal_fence(struct dma_fence *fence);
bool santos_sync_fence_is_signaled(struct dma_fence *fence);
int santos_sync_update_all(void);

/* Provider association lifecycle (M3 integration). */
int santos_sync_set_provider_ops(const struct santos_sync_provider_ops *ops);
int santos_sync_clear_provider_ops(void);
int santos_sync_alloc_bind(struct file *file, void *syncinfo, int state);

/*
 * M3 pre-submit alloc reservation.  claim() validates the exact alloc
 * fops and atomically rejects consumed/claimed/bound allocs; unclaim()
 * drops an uncommitted reservation; a successful BOUND_PENDING bind
 * requires the claim and consumes it (-EPERM otherwise).
 */
int santos_sync_alloc_claim(struct file *file);
void santos_sync_alloc_unclaim(struct file *file);

#endif /* _SANTOS_SYNC_CORE_H */
