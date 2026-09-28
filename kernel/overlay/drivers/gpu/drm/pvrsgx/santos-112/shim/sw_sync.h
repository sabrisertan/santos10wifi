// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S3 skeleton shim for the Android sync_framework UAPI.
 *
 * Golden queue.c/deviceclass.c use sw_sync_timeline_*,
 * sw_sync_pt_create, sync_fence_{create,put,install}, sync_pt_free
 * and sync_timeline_destroy from the old Android sync framework,
 * which does not exist in Linux 7.2 (dma_fence/sync_file replaced
 * it). The real dma_fence port lands in S5 (1.17 C-oracle).
 *
 * This header declares the exact signatures the core calls; the
 * matching no-op definitions live in santos-stub-sync.c. Opaque
 * pointers only — no struct layouts, no semantics. Skeleton links,
 * never runs (no hardware probe yet).
 */
#ifndef SANTOS_SHIM_SW_SYNC_H
#define SANTOS_SHIM_SW_SYNC_H

#include <linux/types.h>

struct sw_sync_timeline;
struct sync_pt;
struct sync_fence;

struct sw_sync_timeline *sw_sync_timeline_create(const char *name);
void sw_sync_timeline_inc(struct sw_sync_timeline *obj, __u32 value);
void sync_timeline_destroy(struct sw_sync_timeline *obj);
struct sync_pt *sw_sync_pt_create(struct sw_sync_timeline *obj,
				  __u32 value);
void sync_pt_free(struct sync_pt *pt);
struct sync_fence *sync_fence_create(const char *name, struct sync_pt *pt);
void sync_fence_put(struct sync_fence *fence);
int sync_fence_install(struct sync_fence *fence, int fd);

#endif /* SANTOS_SHIM_SW_SYNC_H */
