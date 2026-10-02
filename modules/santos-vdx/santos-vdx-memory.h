/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SANTOS_VDX_MEMORY_H
#define SANTOS_VDX_MEMORY_H

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

/* BO/RENDEC storage is CPU-virtually contiguous, not necessarily physically
 * contiguous. Both the userspace mapping and firmware MMU must use the same
 * backing page. virt_to_page() alone is invalid for vmap-backed storage.
 * Storage remains pinned by the existing BO/engine lifetime rules. */
static inline void *santos_vdx_cpu_alloc(size_t size)
{
	/* Large slab allocations round each 1080p surface up to a power of
	 * two and still attempt a high-order allocation. Allocate only the
	 * required pages instead; one-page command BOs retain the cheap
	 * direct-map path. kvfree() pairs with both storage types. */
	if (size > PAGE_SIZE)
		return vzalloc(size);
	return kzalloc(size, GFP_KERNEL);
}

static inline struct page *santos_vdx_cpu_page(const void *address)
{
	if (is_vmalloc_addr(address))
		return vmalloc_to_page(address);
	return virt_to_page(address);
}

#endif
