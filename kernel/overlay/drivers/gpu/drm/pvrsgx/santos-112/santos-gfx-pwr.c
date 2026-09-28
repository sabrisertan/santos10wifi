// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos A122 - GFX (00:02.0) PCI power-state probe + optional D0 restore.
 *
 * Context (source):
 *  - Golden 3.4 vendor tree has no mid_power_off_devices() PCI fixup and its
 *    psb_probe() calls pci_enable_device(00:02.0) (D0) before the first SGX
 *    access in psb_driver_load().
 *  - The 7.2 tree (mainline MID) applies DECLARE_PCI_FIXUP_FINAL
 *    mid_power_off_devices() to every Intel PCI function with a valid
 *    LSS-type vendor capability: it sets PMCSR D3hot at boot
 *    (arch/x86/pci/intel_mid.c). On santos10 the mainline pwr.c driver never
 *    binds (IDs 0x0828/0x11a1 != 0x08ec) and pci-mid.c does not enable the
 *    platform PM hook (CPU model 0x35 not in its table), so any D3hot set by
 *    the fixup would persist unless a driver enables the device.
 *
 * This probe is READ-ONLY by default (do_up=0). With do_up=1 it restores
 * PCI_D0 through the PCI core and re-reads. ZERO SGX MMIO here; the single
 * SGX read remains santos-sgx-one.ko, gated on the frozen result.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>

static int do_up;
module_param(do_up, int, 0444);
MODULE_PARM_DESC(do_up, "0 = read-only probe, 1 = pci_set_power_state(D0)");

static void gfx_show(struct pci_dev *gfx, const char *tag)
{
	u16 pmcsr = 0xffff, cmd = 0xffff;
	int pm = pci_find_capability(gfx, PCI_CAP_ID_PM);
	int vndr = pci_find_capability(gfx, PCI_CAP_ID_VNDR);
	u8 ss = 0;

	if (pm)
		pci_read_config_word(gfx, pm + PCI_PM_CTRL, &pmcsr);
	pci_read_config_word(gfx, PCI_COMMAND, &cmd);
	if (vndr)
		pci_read_config_byte(gfx, vndr + 4, &ss);

	pr_info("santos-gfx-pwr: %s pm_cap=%d vndr_cap=%d ss=0x%02x pmcsr=0x%04x pci_state=%d cmd=0x%04x\n",
		tag, pm, vndr, ss, pmcsr, gfx->current_state, cmd);
}

static int __init santos_gfx_pwr_init(void)
{
	struct pci_dev *gfx;
	int rc = 0;

	pr_info("santos-gfx-pwr: start do_up=%d\n", do_up);

	gfx = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 0));
	if (!gfx) {
		pr_err("santos-gfx-pwr: no 00:02.0\n");
		return -ENODEV;
	}

	gfx_show(gfx, "BEFORE");

	if (do_up) {
		rc = pci_set_power_state(gfx, PCI_D0);
		pr_info("santos-gfx-pwr: pci_set_power_state(D0) rc=%d\n", rc);
		msleep(20);
		gfx_show(gfx, "AFTER");
	}

	pr_info("santos-gfx-pwr: verdict=%s\n", do_up ? "UP_APPLIED" : "CENSUS");
	pci_dev_put(gfx);
	return 0;
}

static void __exit santos_gfx_pwr_exit(void)
{
	pr_info("santos-gfx-pwr: unload (state left as-is)\n");
}

module_init(santos_gfx_pwr_init);
module_exit(santos_gfx_pwr_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("A122 GFX 00:02.0 PCI power-state probe + optional D0 restore");
MODULE_LICENSE("GPL");
