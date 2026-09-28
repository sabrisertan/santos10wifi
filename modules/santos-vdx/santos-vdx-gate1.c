// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos VDX Gate 1 — MSVDX device/power/IRQ aliveness, staged and explicit.
 *
 * Stages (all default 0; nothing but the P-Unit APM census runs by default):
 *   apm_field=N   APM island field index (0=gfx, 1=video-dec, 2=video-enc,
 *                 3=gl3, 4=isp, 5=iph); default 1 = video-dec.
 *   do_up=1       clear the selected APM field (island UP) and wait for STS.
 *   do_down_up=1  set field to 3 (DOWN), wait, then clear to 0 (UP), wait.
 *                 Proves writability; only run if do_up alone showed no change.
 *   do_vdc=1      ioremap BAR0 and read VDC identity/enable/mask + MSVDX clock
 *                 gating. No VDC writes.
 *   do_msvdx=1    read MSVDX ID/REV/status and bounded-wait for the punit
 *                 firmware signature (0xA5A5A5A5) + FW status/id.
 *   do_irq=1      clear MSVDX IRQ status, enable the VDC MSVDX interrupt bit,
 *                 read back, then mask it again. No handler registration yet.
 *
 * This module never writes MSVDX MMIO except the explicitly gated do_irq
 * clearing above. It does not submit commands, does not touch the VDX MMU,
 * does not reset the core and does not power the island down.
 *
 * Register provenance: exact 3.4 sources
 *   drivers/staging/intel_media/video/decode/psb_msvdx_reg.h
 *   drivers/staging/intel_media/common/psb_drv.h (VDC INT registers)
 * APM/mSGBUS access provenance: A122 santos-pvr-power.c (proven on 7.2).
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
#define SANTOS_APMBA		0x7a

#define SANTOS_APM_CMD		0x00
#define SANTOS_APM_STS		0x04
#define SANTOS_APM_FIELDS	6
#define SANTOS_APM_FIELD_MASK	0x3

#define SANTOS_GFX_PCI_VID	0x8086
#define SANTOS_GFX_PCI_DID	0x08c8

#define SANTOS_BAR0_LEN		0x100000
#define SANTOS_VDC_SIZE		0x80000
#define SANTOS_MSVDX_OFFSET	0x90000
#define SANTOS_MSVDX_SIZE	0x10000

#define SANTOS_VDC_INT_ENABLE	0x20A0
#define SANTOS_VDC_INT_MASK	0x20A8
#define SANTOS_VDC_INT_IDENTITY	0x20A4
#define SANTOS_VDC_MSVDX_FLAG	(1 << 19)
#define SANTOS_VDC_MSVDX_CLKGT	0x2064

#define SANTOS_MTX_ENABLE	0x0000
#define SANTOS_MSVDX_INT_STATUS	0x0608
#define SANTOS_MSVDX_INT_CLEAR	0x060C
#define SANTOS_MSVDX_HOST_INT_EN	0x0610
#define SANTOS_MSVDX_MTX_IRQ_MASK	0x00004000
#define SANTOS_MSVDX_CLK_ENABLE	0x0620
#define SANTOS_MSVDX_CORE_ID	0x0630
#define SANTOS_MSVDX_CORE_REV	0x0640
#define SANTOS_MSVDX_MMU_STATUS	0x068C
#define SANTOS_MSVDX_FW_STATUS	0x2FD0
#define SANTOS_MSVDX_FW_ID	0x2FD4
#define SANTOS_MSVDX_MSG_COUNTER	0x2FDC
#define SANTOS_MSVDX_SIGNATURE	0x2FE0
#define SANTOS_MSVDX_SIGNATURE_VALUE	0xA5A5A5A5

static int apm_field = 1;
module_param(apm_field, int, 0444);
MODULE_PARM_DESC(apm_field, "APM island field index (0=gfx,1=vdec,2=venc,3=gl3,4=isp,5=iph)");

static int do_up;
module_param(do_up, int, 0444);
MODULE_PARM_DESC(do_up, "1 = clear selected APM field (island UP) and wait");

static int do_down_up;
module_param(do_down_up, int, 0444);
MODULE_PARM_DESC(do_down_up, "1 = DOWN then UP write proof for the selected field");

static int do_vdc;
module_param(do_vdc, int, 0444);
MODULE_PARM_DESC(do_vdc, "1 = read VDC identity/enable/mask + clock gating");

static int do_msvdx;
module_param(do_msvdx, int, 0444);
MODULE_PARM_DESC(do_msvdx, "1 = read MSVDX ID/REV/status + wait FW signature");

static int do_irq;
module_param(do_irq, int, 0444);
MODULE_PARM_DESC(do_irq, "1 = clear/enable/readback/mask the VDC MSVDX IRQ bit");

static DEFINE_SPINLOCK(santos_msgbus_lock);
static struct pci_dev *santos_host;

static u32 santos_msgbus_read32(u8 port, u32 addr)
{
	unsigned long flags;
	u32 data, cmd, cmdext;

	cmd = (SANTOS_MSGBUS_READ << 24) | (port << 16) |
		((addr & 0xff) << 8) | SANTOS_MSGBUS_DWORD_EN;
	cmdext = addr & 0xffffff00;

	spin_lock_irqsave(&santos_msgbus_lock, flags);
	if (cmdext)
		pci_write_config_dword(santos_host, SANTOS_MSGBUS_CTRL_EXT, cmdext);
	pci_write_config_dword(santos_host, SANTOS_MSGBUS_CTRL, cmd);
	pci_read_config_dword(santos_host, SANTOS_MSGBUS_DATA, &data);
	spin_unlock_irqrestore(&santos_msgbus_lock, flags);
	return data;
}

static u32 apm_field_get(u32 sts, int idx)
{
	return (sts >> (2 * idx)) & SANTOS_APM_FIELD_MASK;
}

static int apm_wait_field(unsigned long base, int idx, u32 val)
{
	int i;

	for (i = 0; i < 500000; i++) {
		if (apm_field_get(inl(base + SANTOS_APM_STS), idx) == val)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static void apm_dump(const char *tag, unsigned long base)
{
	u32 sts = inl(base + SANTOS_APM_STS);
	u32 cmd = inl(base + SANTOS_APM_CMD);
	int i;

	pr_info("santos-vdx-gate1: %s APM_STS=0x%08x APM_CMD=0x%08x\n",
		tag, sts, cmd);
	for (i = 0; i < SANTOS_APM_FIELDS; i++)
		pr_info("santos-vdx-gate1:   field[%d] sts=%u cmd=%u\n",
			i, apm_field_get(sts, i), apm_field_get(cmd, i));
}

static int gate1_apm(void)
{
	u32 apm_base_u32;
	unsigned long apm_base;
	struct pci_dev *host;
	u32 sts, want;

	host = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!host) {
		pr_err("santos-vdx-gate1: no 00:00.0 (msgbus host)\n");
		return -ENODEV;
	}
	santos_host = host;

	apm_base_u32 = santos_msgbus_read32(SANTOS_PUNIT_PORT, SANTOS_APMBA);
	apm_base = apm_base_u32 & 0xffff;
	pr_info("santos-vdx-gate1: APMBA=0x%08x apm_base=0x%lx\n",
		apm_base_u32, apm_base);
	if (!apm_base) {
		pr_err("santos-vdx-gate1: APM base zero, stop\n");
		return -EIO;
	}

	apm_dump("BEFORE", apm_base);

	if (do_down_up) {
		sts = inl(apm_base + SANTOS_APM_STS);
		want = (sts & ~(SANTOS_APM_FIELD_MASK << (2 * apm_field))) |
			(SANTOS_APM_FIELD_MASK << (2 * apm_field));
		pr_info("santos-vdx-gate1: DOWN write APM_CMD=0x%08x field[%d]=3\n",
			want, apm_field);
		outl(want, apm_base + SANTOS_APM_CMD);
		if (apm_wait_field(apm_base, apm_field, 3))
			return -ETIMEDOUT;
		apm_dump("AFTER_DOWN", apm_base);
	}

	if (do_up || do_down_up) {
		sts = inl(apm_base + SANTOS_APM_STS);
		want = sts & ~(SANTOS_APM_FIELD_MASK << (2 * apm_field));
		if (want == sts) {
			pr_info("santos-vdx-gate1: field[%d] already clear, no UP write\n",
				apm_field);
		} else {
			pr_info("santos-vdx-gate1: UP write APM_CMD=0x%08x field[%d]=0\n",
				want, apm_field);
			outl(want, apm_base + SANTOS_APM_CMD);
		}
		if (apm_wait_field(apm_base, apm_field, 0))
			return -ETIMEDOUT;
		apm_dump("AFTER_UP", apm_base);
	}

	pci_dev_put(host);
	santos_host = NULL;
	return 0;
}

static int gate1_mmio(void)
{
	struct pci_dev *gfx;
	void __iomem *bar0;
	u32 id, rev, int_sts, mmu_sts, clkgt;
	int i, ret = 0;

	gfx = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 0));
	if (!gfx) {
		pr_err("santos-vdx-gate1: no 00:02.0\n");
		return -ENODEV;
	}
	if (!(pci_resource_flags(gfx, 0) & IORESOURCE_MEM)) {
		pr_err("santos-vdx-gate1: BAR0 not MEM\n");
		pci_dev_put(gfx);
		return -EIO;
	}

	bar0 = pci_iomap(gfx, 0, SANTOS_BAR0_LEN);
	if (!bar0) {
		pr_err("santos-vdx-gate1: BAR0 iomap failed\n");
		pci_dev_put(gfx);
		return -EIO;
	}

	pr_info("santos-vdx-gate1: BAR0 mapped len=0x%x VDC=+0x0 MSVDX=+0x%x\n",
		SANTOS_BAR0_LEN, SANTOS_MSVDX_OFFSET);

	if (do_vdc || do_irq) {
		u32 en = ioread32(bar0 + SANTOS_VDC_INT_ENABLE);
		u32 mk = ioread32(bar0 + SANTOS_VDC_INT_MASK);
		u32 idn = ioread32(bar0 + SANTOS_VDC_INT_IDENTITY);
		clkgt = ioread32(bar0 + SANTOS_VDC_MSVDX_CLKGT);
		pr_info("santos-vdx-gate1: VDC EN=0x%08x MASK=0x%08x ID=0x%08x msvdx_bit=%u\n",
			en, mk, idn, !!(idn & SANTOS_VDC_MSVDX_FLAG));
		pr_info("santos-vdx-gate1: VDC 0x2064 clkgating=0x%08x\n", clkgt);
	}

	/*
	 * MSVDX-side interrupt path only. The VDC routing bits are owned by
	 * the live PVR Services stack; Gate 1 must not change them. The host
	 * interrupt enable is restored to 0 because no handler is installed
	 * until Gate 2.
	 */
	if (do_irq) {
		u32 st0 = ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_INT_STATUS);

		pr_info("santos-vdx-gate1: IRQ INT_STATUS before=0x%08x\n", st0);
		iowrite32(SANTOS_MSVDX_MTX_IRQ_MASK,
			  bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_INT_CLEAR);
		pr_info("santos-vdx-gate1: IRQ INT_STATUS after MTX_IRQ clear=0x%08x\n",
			ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_INT_STATUS));
		iowrite32(SANTOS_MSVDX_MTX_IRQ_MASK,
			  bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_HOST_INT_EN);
		pr_info("santos-vdx-gate1: IRQ HOST_INT_ENABLE after set=0x%08x\n",
			ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_HOST_INT_EN));
		iowrite32(0, bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_HOST_INT_EN);
		pr_info("santos-vdx-gate1: IRQ HOST_INT_ENABLE restored=0x%08x\n",
			ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_HOST_INT_EN));
	}

	if (do_msvdx) {
		u32 clken = ioread32(bar0 + SANTOS_MSVDX_OFFSET +
				     SANTOS_MSVDX_CLK_ENABLE);
		pr_info("santos-vdx-gate1: FIRST TOUCH MSVDX_MAN_CLK_ENABLE(0x620)=0x%08x\n",
			clken);
		id = ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_CORE_ID);
		rev = ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_CORE_REV);
		int_sts = ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_INT_STATUS);
		mmu_sts = ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_MMU_STATUS);
		pr_info("santos-vdx-gate1: MSVDX CORE_ID=0x%08x CORE_REV=0x%08x\n",
			id, rev);
		pr_info("santos-vdx-gate1: MSVDX INT_STATUS=0x%08x MMU_STATUS=0x%08x\n",
			int_sts, mmu_sts);
		pr_info("santos-vdx-gate1: MTX_ENABLE=0x%08x\n",
			ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MTX_ENABLE));

		for (i = 0; i < 1000; i++) {
			u32 sig = ioread32(bar0 + SANTOS_MSVDX_OFFSET +
					   SANTOS_MSVDX_SIGNATURE);
			if (sig == SANTOS_MSVDX_SIGNATURE_VALUE) {
				pr_info("santos-vdx-gate1: FW signature OK after %d polls "
					"(FW_STATUS=0x%08x FW_ID=0x%08x MSG_COUNTER=0x%08x)\n",
					i,
					ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_FW_STATUS),
					ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_FW_ID),
					ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_MSG_COUNTER));
				break;
			}
			usleep_range(1000, 2000);
		}
		if (i >= 1000) {
			pr_err("santos-vdx-gate1: FW signature TIMEOUT (last=0x%08x)\n",
			       ioread32(bar0 + SANTOS_MSVDX_OFFSET + SANTOS_MSVDX_SIGNATURE));
			ret = -ETIMEDOUT;
		}
	}

	pci_iounmap(gfx, bar0);
	pci_dev_put(gfx);
	return ret;
}

static int __init santos_vdx_gate1_init(void)
{
	int ret;

	pr_info("santos-vdx-gate1: start apm_field=%d do_up=%d do_down_up=%d "
		"do_vdc=%d do_msvdx=%d do_irq=%d\n",
		apm_field, do_up, do_down_up, do_vdc, do_msvdx, do_irq);

	if (apm_field < 0 || apm_field >= SANTOS_APM_FIELDS) {
		pr_err("santos-vdx-gate1: bad apm_field\n");
		return -EINVAL;
	}

	ret = gate1_apm();
	if (ret)
		pr_err("santos-vdx-gate1: APM stage ret=%d\n", ret);

	if (do_vdc || do_msvdx || do_irq) {
		int r2 = gate1_mmio();
		if (!ret)
			ret = r2;
	}

	pr_info("santos-vdx-gate1: done ret=%d\n", ret);
	return ret;
}

static void __exit santos_vdx_gate1_exit(void)
{
	pr_info("santos-vdx-gate1: unload (island left as-is)\n");
}

module_init(santos_vdx_gate1_init);
module_exit(santos_vdx_gate1_exit);

MODULE_AUTHOR("Santos10wifi VDX7 phase");
MODULE_DESCRIPTION("Santos VDX Gate 1: staged MSVDX power/MMIO/FW aliveness probe");
MODULE_LICENSE("GPL");
