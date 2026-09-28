// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S5c power v3 — param-driven island experiment (M4).
 *
 * Lessons: (a) APM_STS=0 reads "all clear" on 7.2 vs 0xfff on golden
 * (working) — NEVER infer ON from zeros; (b) writel posts, only reads
 * prove; (c) golden-algorithm conditional write would skip a 0 state,
 * so the field value is an explicit parameter.
 *
 * insmod santos-pvr-power.ko [do_write=0|1] [gfx_val=0..3]
 *   do_write=0 (default): read-only census (APM_STS/CMD, OSPM_SSS/SSC).
 *   do_write=1: RMW APM_CMD setting ONLY the graphics D0I3 field
 *     (bits[1:0]) to gfx_val, bounded wait for match, then (only on
 *     match) ioremap BAR0 once + single CORE_REV read + unmap.
 * DISPLAY (OSPM block) is never written. Leave-ON, no power-down.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/spinlock.h>

#define SANTOS_MSGBUS_CTRL	0xD0
#define SANTOS_MSGBUS_DATA	0xD4
#define SANTOS_MSGBUS_CTRL_EXT	0xD8
#define SANTOS_MSGBUS_READ	0x10
#define SANTOS_MSGBUS_DWORD_EN	0xf0
#define SANTOS_PUNIT_PORT	0x04
#define SANTOS_OSPMBA		0x78
#define SANTOS_APMBA		0x7a

#define SANTOS_APM_STS		0x04
#define SANTOS_APM_CMD		0x00
#define SANTOS_OSPM_SSS		0x30
#define SANTOS_OSPM_SSC		0x20
#define SANTOS_GFX_D0I3_MASK	0x3

#define SANTOS_EUR_CR_CORE_REVISION	0x0024

static int do_write;
module_param(do_write, int, 0444);
MODULE_PARM_DESC(do_write, "1 = perform the graphics-field write, 0 = census only");

static int gfx_val = -1;
module_param(gfx_val, int, 0444);
MODULE_PARM_DESC(gfx_val, "D0I3 field value 0..3 for graphics (needs do_write=1)");

static int noread;
module_param(noread, int, 0444);
MODULE_PARM_DESC(noread, "1 = skip the SGX MMIO read (stroke-only for DOWN->UP cycling)");

static DEFINE_SPINLOCK(santos_msgbus_lock);
static struct pci_dev *santos_root;

static u32 santos_msgbus_read32(u8 port, u32 addr)
{
	unsigned long flags;
	u32 data, cmd, cmdext;

	cmd = (SANTOS_MSGBUS_READ << 24) | (port << 16) |
		((addr & 0xff) << 8) | SANTOS_MSGBUS_DWORD_EN;
	cmdext = addr & 0xffffff00;

	spin_lock_irqsave(&santos_msgbus_lock, flags);
	if (cmdext)
		pci_write_config_dword(santos_root,
				       SANTOS_MSGBUS_CTRL_EXT, cmdext);
	pci_write_config_dword(santos_root, SANTOS_MSGBUS_CTRL, cmd);
	pci_read_config_dword(santos_root, SANTOS_MSGBUS_DATA, &data);
	spin_unlock_irqrestore(&santos_msgbus_lock, flags);
	return data;
}

static int __init santos_pvr_power_init(void)
{
	u32 apm_base, want, got;
	unsigned long sts_port, cmd_port;
	int count = 0;
	struct pci_dev *gfx;
	void __iomem *mmio;
	u32 rev;

	pr_info("santos-pwr: v3 start (do_write=%d gfx_val=%d)\n",
		do_write, gfx_val);

	santos_root = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!santos_root) {
		pr_err("santos-pwr: no 00:00.0 FAIL\n");
		return -ENODEV;
	}

	apm_base = santos_msgbus_read32(SANTOS_PUNIT_PORT, SANTOS_APMBA)
		& 0xffff;
	pr_info("santos-pwr: apm_base=0x%x\n", apm_base);
	if (!apm_base) {
		pr_err("santos-pwr: APM base insane, stop\n");
		goto out_put;
	}

	sts_port = apm_base + SANTOS_APM_STS;
	cmd_port = apm_base + SANTOS_APM_CMD;
	pr_info("santos-pwr: APM_STS=0x%08x APM_CMD=0x%08x\n",
		inl(sts_port), inl(cmd_port));

	if (!do_write || gfx_val < 0 || gfx_val > 3) {
		pr_info("santos-pwr: census only, no writes\n");
		goto out_put;
	}

	want = (inl(sts_port) & ~SANTOS_GFX_D0I3_MASK) |
		((u32)gfx_val & SANTOS_GFX_D0I3_MASK);
	pr_info("santos-pwr: writing APM_CMD=0x%08x (gfx field %d)\n",
		want, gfx_val);
	outl(want, cmd_port);
	while (true) {
		got = inl(sts_port);
		if ((got & SANTOS_GFX_D0I3_MASK) ==
		    ((u32)gfx_val & SANTOS_GFX_D0I3_MASK))
			break;
		udelay(10);
		if (++count > 500000) {
			pr_err("santos-pwr: field never matched (sts=0x%08x), NO SGX MMIO\n",
			       got);
			goto out_put;
		}
	}
	pr_info("santos-pwr: field matched after %d polls, sts=0x%08x\n",
		count, inl(sts_port));

	if (noread) {
		pr_info("santos-pwr: noread, stopping before SGX MMIO\n");
		goto out_put;
	}
	gfx = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 0));
	if (!gfx) {
		pr_err("santos-pwr: no 00:02.0, NO SGX MMIO\n");
		goto out_put;
	}
	if (!(pci_resource_flags(gfx, 0) & IORESOURCE_MEM)) {
		pr_err("santos-pwr: BAR0 not MEM, NO SGX MMIO\n");
		goto out_put_gfx;
	}
	mmio = pci_iomap(gfx, 0, 0x90000);
	if (!mmio) {
		pr_err("santos-pwr: BAR0 iomap failed, NO SGX MMIO\n");
		goto out_put_gfx;
	}
	rev = readl(mmio + 0x80000 + SANTOS_EUR_CR_CORE_REVISION);
	pr_info("santos-pwr: EUR_CR_CORE_REVISION=0x%08x\n", rev);
	pci_iounmap(gfx, mmio);
out_put_gfx:
	pci_dev_put(gfx);
out_put:
	pci_dev_put(santos_root);
	santos_root = NULL;
	pr_info("santos-pwr: done (island left as-is)\n");
	return 0;
}

static void __exit santos_pvr_power_exit(void)
{
	pr_info("santos-pvr-power: unload (no power-down by design)\n");
}

module_init(santos_pvr_power_init);
module_exit(santos_pvr_power_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("S5c power v3: param-driven island experiment (M4)");
MODULE_LICENSE("GPL");
