// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S3 skeleton shim for the Android sync UAPI header <sync.h>.
 *
 * Only consumer is bridged_pvr_bridge.c (native-window sync fence
 * install). Real dma_fence-backed implementation lands in S5 with
 * the pvr_sync port. Skeleton links, never runs.
 */
#ifndef SANTOS_SHIM_SYNC_UAPI_H
#define SANTOS_SHIM_SYNC_UAPI_H

struct sync_fence;

int sync_fence_install(struct sync_fence *fence, int fd);

#endif /* SANTOS_SHIM_SYNC_UAPI_H */
