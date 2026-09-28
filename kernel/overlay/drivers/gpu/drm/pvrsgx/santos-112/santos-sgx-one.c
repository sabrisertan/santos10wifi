// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos A121 - single first SGX MMIO read (post-precondition boundary).
 *
 * Loaded ONLY after santos-pmu-preinit.ko has applied the golden SCU PMU
 * sequence and userspace has frozen the resulting state to persistent
 * storage. This module performs exactly one SGX register read:
 *
 *   BAR0(00:02.0) + 0x80000 (SGX window) + EUR_CR_CORE_REVISION (0x0024)
 *
 * This is the same access shape that wedged the 7.2 boot in all previous
 * attempts (A104 census, S5c power v3). If the SoC hangs, the frozen
 * precondition state from the previous phase is the evidence.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

#define SANTOS_SGX_OFFSET	0x80000
#define SANTOS_EUR_CR_CORE_REVISION	0x0024
#define SANTOS_EUR_CR_CORE_ID		0x0020

static int __init santos_sgx_one_init(void)
{
	struct pci_dev *gfx;
	void __iomem *mmio;
	u32 rev;

	pr_info("santos-sgx-one: start (single first SGX MMIO read)\n");

	gfx = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 0));
	if (!gfx) {
		pr_err("santos-sgx-one: no 00:02.0\n");
		return -ENODEV;
	}
	if (!(pci_resource_flags(gfx, 0) & IORESOURCE_MEM)) {
		pr_err("santos-sgx-one: BAR0 not MEM, no read\n");
		pci_dev_put(gfx);
		return -EINVAL;
	}

	mmio = pci_iomap(gfx, 0, 0x90000);
	if (!mmio) {
		pr_err("santos-sgx-one: BAR0 iomap failed, no read\n");
		pci_dev_put(gfx);
		return -ENOMEM;
	}

	pr_info("santos-sgx-one: about to readl SGX+0x%x (EUR_CR_CORE_REVISION)\n",
		SANTOS_SGX_OFFSET + SANTOS_EUR_CR_CORE_REVISION);
	rev = readl(mmio + SANTOS_SGX_OFFSET + SANTOS_EUR_CR_CORE_REVISION);
	pr_info("santos-sgx-one: EUR_CR_CORE_REVISION=0x%08x (SURVIVED)\n", rev);

	pci_iounmap(gfx, mmio);
	pci_dev_put(gfx);
	return 0;
}

static void __exit santos_sgx_one_exit(void)
{
	pr_info("santos-sgx-one: unload\n");
}

module_init(santos_sgx_one_init);
module_exit(santos_sgx_one_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("A121 single first SGX MMIO read (post-precondition)");
MODULE_LICENSE("GPL");
