/* SPDX-License-Identifier: GPL-2.0-only
 * Santos VDX Gate 3 compat: minimal 3.4 DRM/TTM surface for the ported
 * psb_mmu.c / psb_msvdx*.c hardware code. See GATE3-PORT-MAP.md.
 */
#ifndef SANTOS_VDX_COMPAT_H
#define SANTOS_VDX_COMPAT_H

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/highmem.h>
#include <linux/vmalloc.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <asm/cpuid/api.h>
#include <drm/drm_device.h>
#include <drm/drm_print.h>

#define PSB_MMU_CACHED_MEMORY	  0x0001
#define PSB_MMU_RO_MEMORY	  0x0002
#define PSB_MMU_WO_MEMORY	  0x0004

#define PSB_PDE_MASK		  0x003FFFFF
#define PSB_PDE_SHIFT		  22
#define PSB_PTE_SHIFT		  12

#define PSB_PTE_VALID		  0x0001
#define PSB_PTE_WO		  0x0002
#define PSB_PTE_RO		  0x0004
#define PSB_PTE_CACHED		  0x0008
#define PSB_UDELAY(us)		udelay(us)

enum mmu_type_t {
	IMG_MMU = 1,
	VSP_MMU = 2
};

struct psb_mmu_driver;
struct psb_mmu_pd;

struct drm_psb_private {
	struct drm_device *dev;
	void __iomem *msvdx_reg;
	atomic_t msvdx_mmu_invaldc;
	atomic_t topaz_mmu_invaldc;
	struct psb_mmu_driver *mmu;
	void *msvdx_private;
};

struct psb_mmu_driver *psb_mmu_driver_init(uint8_t __iomem *registers,
		int trap_pagefaults, int invalid_type,
		struct drm_psb_private *dev_priv, enum mmu_type_t mmu_type);
void psb_mmu_driver_takedown(struct psb_mmu_driver *driver);
struct psb_mmu_pd *psb_mmu_get_default_pd(struct psb_mmu_driver *driver);
struct psb_mmu_pd *psb_mmu_alloc_pd(struct psb_mmu_driver *driver,
		int trap_pagefaults, int invalid_type);
void psb_mmu_free_pagedir(struct psb_mmu_pd *pd);
void psb_mmu_flush(struct psb_mmu_driver *driver, int rc_prot);
void psb_mmu_remove_pfn_sequence(struct psb_mmu_pd *pd,
		unsigned long address, uint32_t num_pages);
int psb_mmu_insert_pfn_sequence(struct psb_mmu_pd *pd, uint32_t start_pfn,
		unsigned long address, uint32_t num_pages, int type);
void psb_mmu_set_pd_context(struct psb_mmu_pd *pd, int hw_context);
int psb_mmu_insert_pages(struct psb_mmu_pd *pd, struct page **pages,
		unsigned long address, uint32_t num_pages,
		uint32_t desired_tile_stride, uint32_t hw_tile_stride,
		int type);
void psb_mmu_remove_pages(struct psb_mmu_pd *pd, unsigned long address,
		uint32_t num_pages, uint32_t desired_tile_stride,
		uint32_t hw_tile_stride);
uint32_t psb_get_default_pd_addr(struct psb_mmu_driver *driver);

#endif
