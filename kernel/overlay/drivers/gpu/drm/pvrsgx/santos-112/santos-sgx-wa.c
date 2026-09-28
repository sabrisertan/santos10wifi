// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos A123 - golden pre-first-read SGX WA sequence on 7.2.
 *
 * Exact golden source sequence (kernel-santos10 psb_drv.c, Cloverview build
 * with SGX_FEATURE_MP_CORE_COUNT == 2; verified against psb_drv.h):
 *
 *   sgx_reg = ioremap(BAR0(00:02.0) + MRST_SGX_OFFSET(0x80000), PSB_SGX_SIZE(0x8000))
 *   [IS_CTP_NEED_WA: device id 0x08c8]
 *   iowrite32(0x1,   sgx_reg + 0x4000);   -> BAR0 + 0x84000
 *   iowrite32(0x5,   sgx_reg + 0x4004);   -> BAR0 + 0x84004
 *   iowrite32(0xa,   sgx_reg + 0x4004);   -> BAR0 + 0x84004
 *   iowrite32(0x2aa, sgx_reg + 0x4020);   -> BAR0 + 0x84020
 *
 * The first SGX READ is EUR_CR_CORE_REVISION (0x0024) -> BAR0 + 0x80024.
 *
 * Posted-write semantics: iowrite32() returning does not prove the SGX target
 * responded. The decisive positive outcome is the CORE_REV read surviving
 * after the complete four-write sequence. This module performs exactly that
 * and nothing else. ZERO SGX access unless do_wa=1.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>

#define SANTOS_GFX_DEVICE	0x08c8
#define SANTOS_BAR0_LEN		0x90000
#define SANTOS_SGX_OFFSET	0x80000	/* MRST_SGX_OFFSET, IS_MID path */
#define SANTOS_WA_4000		0x4000
#define SANTOS_WA_4004		0x4004
#define SANTOS_WA_4020		0x4020
#define SANTOS_CORE_REVISION	0x0024	/* EUR_CR_CORE_REVISION */

static int do_wa;
module_param(do_wa, int, 0444);
MODULE_PARM_DESC(do_wa, "0 = census only (zero SGX access), 1 = four golden WA writes then CORE_REV read");

static int __init santos_sgx_wa_init(void)
{
	struct pci_dev *gfx;
	void __iomem *mmio;
	u32 rev;

	pr_info("santos-sgx-wa: start do_wa=%d\n", do_wa);

	gfx = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 0));
	if (!gfx) {
		pr_err("santos-sgx-wa: no 00:02.0\n");
		return -ENODEV;
	}
	if ((gfx->device & 0xffff) != SANTOS_GFX_DEVICE) {
		pr_err("santos-sgx-wa: 00:02.0 id %04x != %04x (golden WA condition false)\n",
		       gfx->device, SANTOS_GFX_DEVICE);
		pci_dev_put(gfx);
		return -ENODEV;
	}
	if (!(pci_resource_flags(gfx, 0) & IORESOURCE_MEM)) {
		pr_err("santos-sgx-wa: BAR0 not MEM\n");
		pci_dev_put(gfx);
		return -EINVAL;
	}

	mmio = pci_iomap(gfx, 0, SANTOS_BAR0_LEN);
	if (!mmio) {
		pr_err("santos-sgx-wa: BAR0 iomap failed\n");
		pci_dev_put(gfx);
		return -ENOMEM;
	}

	pr_info("santos-sgx-wa: BAR0=0x%llx sgx_off=0x%x wa_addrs=+0x%x +0x%x +0x%x core_rev=+0x%x\n",
		(unsigned long long)pci_resource_start(gfx, 0), SANTOS_SGX_OFFSET,
		SANTOS_SGX_OFFSET + SANTOS_WA_4000,
		SANTOS_SGX_OFFSET + SANTOS_WA_4004,
		SANTOS_SGX_OFFSET + SANTOS_WA_4020,
		SANTOS_SGX_OFFSET + SANTOS_CORE_REVISION);

	if (!do_wa) {
		pr_info("santos-sgx-wa: census only, ZERO SGX accesses\n");
		pr_info("santos-sgx-wa: verdict=CENSUS\n");
		pci_iounmap(gfx, mmio);
		pci_dev_put(gfx);
		return 0;
	}

	/* Exact golden WA, core-count 2 branch, in source order. */
	iowrite32(0x1, mmio + SANTOS_SGX_OFFSET + SANTOS_WA_4000);
	pr_info("santos-sgx-wa: WA1 iowrite32(0x1, +0x%x) posted\n",
		SANTOS_SGX_OFFSET + SANTOS_WA_4000);
	iowrite32(0x5, mmio + SANTOS_SGX_OFFSET + SANTOS_WA_4004);
	pr_info("santos-sgx-wa: WA2 iowrite32(0x5, +0x%x) posted\n",
		SANTOS_SGX_OFFSET + SANTOS_WA_4004);
	iowrite32(0xa, mmio + SANTOS_SGX_OFFSET + SANTOS_WA_4004);
	pr_info("santos-sgx-wa: WA3 iowrite32(0xa, +0x%x) posted\n",
		SANTOS_SGX_OFFSET + SANTOS_WA_4004);
	iowrite32(0x2aa, mmio + SANTOS_SGX_OFFSET + SANTOS_WA_4020);
	pr_info("santos-sgx-wa: WA4 iowrite32(0x2aa, +0x%x) posted\n",
		SANTOS_SGX_OFFSET + SANTOS_WA_4020);

	pr_info("santos-sgx-wa: about to read EUR_CR_CORE_REVISION (+0x%x)\n",
		SANTOS_SGX_OFFSET + SANTOS_CORE_REVISION);
	rev = readl(mmio + SANTOS_SGX_OFFSET + SANTOS_CORE_REVISION);
	pr_info("santos-sgx-wa: EUR_CR_CORE_REVISION=0x%08x (SURVIVED)\n", rev);
	pr_info("santos-sgx-wa: verdict=WA_READ_SURVIVED\n");

	pci_iounmap(gfx, mmio);
	pci_dev_put(gfx);
	return 0;
}

static void __exit santos_sgx_wa_exit(void)
{
	pr_info("santos-sgx-wa: unload\n");
}

module_init(santos_sgx_wa_init);
module_exit(santos_sgx_wa_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("A123 golden four-write SGX WA then first CORE_REV read");
MODULE_LICENSE("GPL");
