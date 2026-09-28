// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos A121 - 7.2 SCU PMU pre-init reproduction (golden 3.4 pmu_init).
 *
 * Question: is the golden 3.4 SCU PMU (00:02.2) initialization the missing
 * precondition for safe first SGX MMIO on the 7.2 port?
 *
 * This probe reproduces, on 7.2, the hardware-visible sequence that golden
 * 3.4 performs in pmu_init() before psb_driver_load()'s SGX workaround:
 *
 *   1. pm_wkc[0] = WAKE_ENABLE_0, pm_wkc[1] = WAKE_ENABLE_1
 *   2. pm_ics |= 0x100
 *   3. pm_ssc[0..3] = pmu2_states computed by the golden algorithm
 *      (update_all_lss_states + platform_update_all_lss_states)
 *   4. pm_cmd = INTERACTIVE_VALUE (0x00002201), wait busy clear
 *
 * The pmu2_states vector is NOT replayed from the A120 golden measurement.
 * It is computed from live 7.2 PCI state using the golden enumeration
 * (vendor-capability LSS byte), clv_pmu_choose_state() targets and the
 * S0IX/IGNORE masks. Runtime dependencies (current_state, driver bound)
 * are handled explicitly and logged.
 *
 * NO SGX MMIO is performed here. The single SGX read is a separate module
 * (santos-sgx-one.ko) loaded only after userspace has frozen this state.
 *
 * Parameters:
 *   do_init=0 (default) : census only; snapshot + compute + print, no writes.
 *   do_init=1           : apply the sequence above, then snapshot again.
 */
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/string.h>

/* ---- PCI identities ---------------------------------------------------- */
#define SANTOS_PMU_VENDOR	0x8086
#define SANTOS_PMU_DEVICE	0x08ec
#define SANTOS_GFX_VENDOR	0x8086
#define SANTOS_GFX_DEVICE	0x08c0

/* ---- SCU PMU (00:02.2) BAR0 register offsets (golden intel_soc_pmu.h) -- */
#define PM_STS		0x00
#define PM_CMD		0x04
#define PM_ICS		0x08
#define PM_WKC0		0x10
#define PM_WKC1		0x14
#define PM_SSC0		0x20
#define PM_SSC1		0x24
#define PM_SSC2		0x28
#define PM_SSC3		0x2c
#define PM_SSS0		0x30
#define PM_SSS1		0x34
#define PM_SSS2		0x38
#define PM_SSS3		0x3c
#define PM_MSIC		0x58

#define PM_STS_BUSY	0x100
#define PM_ICS_INT_EN	0x100
#define INTERACTIVE_VALUE	0x00002201
#define WAKE_ENABLE_0	0xffffffff
#define WAKE_ENABLE_1	0xffffffff

/* ---- PUNIT msgbus (golden psb_drv.h / santos-pvr-power.c) -------------- */
#define SANTOS_MSGBUS_CTRL	0xD0
#define SANTOS_MSGBUS_DATA	0xD4
#define SANTOS_MSGBUS_CTRL_EXT	0xD8
#define SANTOS_MSGBUS_READ	0x10
#define SANTOS_MSGBUS_DWORD_EN	0xf0
#define SANTOS_PUNIT_PORT	0x04
#define SANTOS_OSPMBA		0x78
#define SANTOS_APMBA		0x7a
#define SANTOS_APM_CMD		0x00
#define SANTOS_APM_STS		0x04
#define SANTOS_OSPM_SSC		0x20
#define SANTOS_OSPM_SSS		0x30

/* ---- Golden LSS constants (intel_soc_clv.h, verbatim) ------------------ */
#define PMU1_MAX_DEVS		8
#define PMU2_MAX_DEVS		55
#define MAX_LSS_POSSIBLE	64
#define LOG_SS_MASK		0x80
#define LOG_ID_MASK		0x7f
#define SSMSK(mask, lss)	((mask) << ((lss) * 2))
#define D0I0_MASK		0
#define D0I1_MASK		1
#define D0I2_MASK		2
#define D0I3_MASK		3

#define PMU_SDIO0_LSS_00	0
#define PMU_EMMC0_LSS_01	1
#define PMU_HSI_LSS_03		3
#define PMU_SECURITY_LSS_04	4
#define PMU_EMMC1_LSS_05	5
#define PMU_USB_OTG_LSS_06	6
#define PMU_USB_HSIC_LSS_07	7
#define PMU_AUDIO_ENGINE_LSS_08	8
#define PMU_AUDIO_DMA_LSS_09	9
#define PMU_SDIO2_LSS_14	14
#define PMU_SPI1_LSS_18		18
#define PMU_I2C0_LSS_20		20
#define PMU_I2C1_LSS_21		21
#define PMU_I2C2_LSS_27		27
#define PMU_SDIO1_LSS_30	30
#define PMU_I2C3_LSS_33		33
#define PMU_I2C4_LSS_34		34
#define PMU_I2C5_LSS_35		35
#define PMU_SPI3_LSS_36		36
#define PMU_UART2_LSS_41	41
#define PMU_AUDIO_SSP0_LSS_51	51
#define PMU_AUDIO_SSP1_LSS_52	52

#define S0IX_TARGET_SSS0_MASK ( \
	SSMSK(D0I3_MASK, PMU_SDIO0_LSS_00) | \
	SSMSK(D0I3_MASK, PMU_EMMC0_LSS_01) | \
	SSMSK(D0I3_MASK, PMU_HSI_LSS_03) | \
	SSMSK(D0I3_MASK, PMU_SECURITY_LSS_04) | \
	SSMSK(D0I3_MASK, PMU_EMMC1_LSS_05) | \
	SSMSK(D0I3_MASK, PMU_USB_OTG_LSS_06) | \
	SSMSK(D0I3_MASK, PMU_USB_HSIC_LSS_07) | \
	SSMSK(D0I3_MASK, PMU_AUDIO_ENGINE_LSS_08) | \
	SSMSK(D0I3_MASK, PMU_AUDIO_DMA_LSS_09) | \
	SSMSK(D0I3_MASK, PMU_SDIO2_LSS_14))

#define S0IX_TARGET_SSS1_MASK ( \
	SSMSK(D0I3_MASK, PMU_SPI1_LSS_18 - 16) | \
	SSMSK(D0I3_MASK, PMU_I2C0_LSS_20 - 16) | \
	SSMSK(D0I3_MASK, PMU_I2C1_LSS_21 - 16) | \
	SSMSK(D0I3_MASK, PMU_I2C2_LSS_27 - 16) | \
	SSMSK(D0I3_MASK, PMU_SDIO1_LSS_30 - 16))

#define S0IX_TARGET_SSS2_MASK ( \
	SSMSK(D0I3_MASK, PMU_I2C3_LSS_33 - 32) | \
	SSMSK(D0I3_MASK, PMU_I2C4_LSS_34 - 32) | \
	SSMSK(D0I3_MASK, PMU_I2C5_LSS_35 - 32) | \
	SSMSK(D0I3_MASK, PMU_SPI3_LSS_36 - 32) | \
	SSMSK(D0I3_MASK, PMU_UART2_LSS_41 - 32))

#define S0IX_TARGET_SSS3_MASK ( \
	SSMSK(D0I3_MASK, PMU_AUDIO_SSP0_LSS_51 - 48) | \
	SSMSK(D0I3_MASK, PMU_AUDIO_SSP1_LSS_52 - 48))

#define IGNORE_SSS0 ( \
	SSMSK(D0I3_MASK, 10) | SSMSK(D0I3_MASK, 11) | \
	SSMSK(D0I3_MASK, 12) | SSMSK(D0I3_MASK, 13) | \
	SSMSK(D0I3_MASK, 15))

#define IGNORE_SSS1 ( \
	SSMSK(D0I3_MASK, 16 - 16) | SSMSK(D0I3_MASK, 17 - 16) | \
	SSMSK(D0I3_MASK, 22 - 16) | SSMSK(D0I3_MASK, 23 - 16) | \
	SSMSK(D0I3_MASK, 24 - 16) | SSMSK(D0I3_MASK, 26 - 16) | \
	SSMSK(D0I3_MASK, 28 - 16) | SSMSK(D0I3_MASK, 29 - 16) | \
	SSMSK(D0I3_MASK, 31 - 16))

#define IGNORE_SSS2 ( \
	SSMSK(D0I3_MASK, 32 - 32) | SSMSK(D0I3_MASK, 37 - 32) | \
	SSMSK(D0I3_MASK, 38 - 32) | SSMSK(D0I3_MASK, 39 - 32) | \
	SSMSK(D0I3_MASK, 42 - 32) | SSMSK(D0I3_MASK, 43 - 32) | \
	SSMSK(D0I3_MASK, 44 - 32) | SSMSK(D0I3_MASK, 45 - 32) | \
	SSMSK(D0I3_MASK, 46 - 32) | SSMSK(D0I3_MASK, 47 - 32))

#define IGNORE_SSS3 ( \
	SSMSK(D0I3_MASK, 53 - 48) | SSMSK(D0I3_MASK, 55 - 48) | \
	SSMSK(D0I3_MASK, 56 - 48) | SSMSK(D0I3_MASK, 57 - 48) | \
	SSMSK(D0I3_MASK, 58 - 48) | SSMSK(D0I3_MASK, 59 - 48) | \
	SSMSK(D0I3_MASK, 60 - 48) | SSMSK(D0I3_MASK, 61 - 48) | \
	SSMSK(D0I3_MASK, 62 - 48) | SSMSK(D0I3_MASK, 63 - 48))

static const u32 s0ix_mask[4] = {
	S0IX_TARGET_SSS0_MASK, S0IX_TARGET_SSS1_MASK,
	S0IX_TARGET_SSS2_MASK, S0IX_TARGET_SSS3_MASK
};
static const u32 ignore_sss[4] = {
	IGNORE_SSS0, IGNORE_SSS1, IGNORE_SSS2, IGNORE_SSS3
};

/* A120 golden pre-WA measurement for direct comparison (not used as input). */
static const u32 golden_ref[4] = {
	0x300f5ecf, 0x00c00f00, 0x000003fc, 0x000033cc
};

static int do_init;	/* 0 = census only, 1 = apply the golden sequence */
module_param(do_init, int, 0444);
MODULE_PARM_DESC(do_init, "0 = snapshot/compute only, 1 = apply golden PMU init");

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
		pci_write_config_dword(santos_root, SANTOS_MSGBUS_CTRL_EXT, cmdext);
	pci_write_config_dword(santos_root, SANTOS_MSGBUS_CTRL, cmd);
	pci_read_config_dword(santos_root, SANTOS_MSGBUS_DATA, &data);
	spin_unlock_irqrestore(&santos_msgbus_lock, flags);
	return data;
}

static pci_power_t clv_pmu_choose_state(int lss)
{
	switch (lss) {
	case PMU_SECURITY_LSS_04:
		return PCI_D2;
	case PMU_USB_OTG_LSS_06:
	case PMU_USB_HSIC_LSS_07:
	case PMU_UART2_LSS_41:
		return PCI_D1;
	default:
		return PCI_D3hot;
	}
}

static int pci_to_platform_state(pci_power_t pci_state)
{
	static const int mask[] = { D0I0_MASK, D0I1_MASK,
				    D0I2_MASK, D0I3_MASK, D0I3_MASK };

	if (pci_state > PCI_D3cold)
		return D0I0_MASK;
	return mask[pci_state];
}

struct santos_lss_state {
	bool present;
	pci_power_t state;
	bool bound;
};

static struct santos_lss_state lss_tab[MAX_LSS_POSSIBLE];

static const char *pci_state_str(pci_power_t s)
{
	switch (s) {
	case PCI_D0: return "D0";
	case PCI_D1: return "D1";
	case PCI_D2: return "D2";
	case PCI_D3hot: return "D3hot";
	case PCI_D3cold: return "D3cold";
	default: return "UNKNOWN";
	}
}

static void santos_enumerate_lss(void)
{
	struct pci_dev *pdev = NULL;
	int count = 0;

	memset(lss_tab, 0, sizeof(lss_tab));

	while ((pdev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, pdev)) != NULL) {
		unsigned int base_class = pdev->class >> 16;
		unsigned int sub_class = (pdev->class & 0xff00) >> 8;
		u8 ss = 0, cap_byte = 0;
		int cap, lss;
		pci_power_t state;
		bool bound = pdev->dev.driver != NULL;

		if (base_class == PCI_BASE_CLASS_BRIDGE)
			continue;

		/* Cloverview LSS byte lives in the vendor capability. */
		cap = pci_find_capability(pdev, PCI_CAP_ID_VNDR);
		if (!cap)
			continue;
		pci_read_config_byte(pdev, cap + 4, &ss);
		pci_read_config_byte(pdev, cap + 5, &cap_byte);

		/* GFX (display, subclass 0) and ISP are PMU1: not in PMU2 vec. */
		if (base_class == PCI_BASE_CLASS_DISPLAY && !sub_class)
			continue;
		if (base_class == PCI_BASE_CLASS_MULTIMEDIA &&
		    sub_class == 0x80)
			continue;

		if (!(ss & LOG_SS_MASK))
			continue;
		lss = ss & LOG_ID_MASK;
		if (lss >= PMU2_MAX_DEVS || lss >= MAX_LSS_POSSIBLE)
			continue;

		state = pdev->current_state;
		if (state == PCI_UNKNOWN)
			state = bound ? PCI_D0 : clv_pmu_choose_state(lss);
		/* Safety refinement over the literal golden algorithm: a device
		 * with a bound driver is in use; never propose a lower-power
		 * state for it than its current one. */
		if (bound && state > PCI_D0)
			state = PCI_D0;

		if (!lss_tab[lss].present) {
			lss_tab[lss].present = true;
			lss_tab[lss].state = state;
			lss_tab[lss].bound = bound;
			count++;
		} else if (state < lss_tab[lss].state) {
			/* weakest/most-active wins over shared LSS */
			lss_tab[lss].state = state;
			lss_tab[lss].bound = lss_tab[lss].bound || bound;
		}

		pr_info("santos-pmu-preinit: pci %s class=%06x cap%02x+4 ss=0x%02x cap=0x%02x lss=%d state=%s bound=%d\n",
			pci_name(pdev), pdev->class, cap, ss, cap_byte, lss,
			pci_state_str(pdev->current_state), bound);
	}

	pr_info("santos-pmu-preinit: lss_devices=%d\n", count);
}

static void santos_compute_vector(u32 vec[4], u32 cfg[4])
{
	int i;

	memset(vec, 0, 4 * sizeof(u32));
	memset(cfg, 0, 4 * sizeof(u32));

	for (i = 0; i < PMU2_MAX_DEVS; i++) {
		if (!lss_tab[i].present)
			continue;
		vec[i / 16] |= (u32)pci_to_platform_state(lss_tab[i].state)
				<< ((i % 16) * 2);
		cfg[i / 16] |= (u32)D0I3_MASK << ((i % 16) * 2);
	}

	for (i = 0; i < 4; i++) {
		vec[i] |= s0ix_mask[i] & ~cfg[i];
		vec[i] &= ~ignore_sss[i];
	}

	pr_info("santos-pmu-preinit: vector computed (golden algorithm):\n");
	pr_info("santos-pmu-preinit:   pm_ssc[0]=0x%08x pci_cfg=0x%08x\n", vec[0], cfg[0]);
	pr_info("santos-pmu-preinit:   pm_ssc[1]=0x%08x pci_cfg=0x%08x\n", vec[1], cfg[1]);
	pr_info("santos-pmu-preinit:   pm_ssc[2]=0x%08x pci_cfg=0x%08x\n", vec[2], cfg[2]);
	pr_info("santos-pmu-preinit:   pm_ssc[3]=0x%08x pci_cfg=0x%08x\n", vec[3], cfg[3]);
	for (i = 0; i < 4; i++)
		pr_info("santos-pmu-preinit:   vs golden A120 [%d]: computed=0x%08x golden=0x%08x %s\n",
			i, vec[i], golden_ref[i],
			vec[i] == golden_ref[i] ? "MATCH" : "DIFF");
}

static void santos_pmu_snapshot(void __iomem *pmu, const char *tag,
				u32 apm_sts, u32 apm_cmd,
				u32 ospm_sss, u32 ospm_ssc)
{
	u32 sts = ioread32(pmu + PM_STS);
	u32 cmd = ioread32(pmu + PM_CMD);
	u32 ics = ioread32(pmu + PM_ICS);
	u32 wkc0 = ioread32(pmu + PM_WKC0);
	u32 wkc1 = ioread32(pmu + PM_WKC1);
	u32 ssc0 = ioread32(pmu + PM_SSC0);
	u32 ssc1 = ioread32(pmu + PM_SSC1);
	u32 ssc2 = ioread32(pmu + PM_SSC2);
	u32 ssc3 = ioread32(pmu + PM_SSC3);
	u32 sss0 = ioread32(pmu + PM_SSS0);
	u32 sss1 = ioread32(pmu + PM_SSS1);
	u32 sss2 = ioread32(pmu + PM_SSS2);
	u32 sss3 = ioread32(pmu + PM_SSS3);
	u32 msic = ioread32(pmu + PM_MSIC);

	pr_info("santos-pmu-preinit: === %s ===\n", tag);
	pr_info("santos-pmu-preinit: pm_sts=0x%08x [rev=0x%02x busy=%u mode_id=0x%x]\n",
		sts, sts & 0xff, !!(sts & PM_STS_BUSY), (sts >> 9) & 0xf);
	pr_info("santos-pmu-preinit: pm_cmd=0x%08x pm_ics=0x%08x pm_msic=0x%08x\n",
		cmd, ics, msic);
	pr_info("santos-pmu-preinit: pm_wkc=0x%08x 0x%08x\n", wkc0, wkc1);
	pr_info("santos-pmu-preinit: pm_ssc=0x%08x 0x%08x 0x%08x 0x%08x\n",
		ssc0, ssc1, ssc2, ssc3);
	pr_info("santos-pmu-preinit: pm_sss=0x%08x 0x%08x 0x%08x 0x%08x\n",
		sss0, sss1, sss2, sss3);
	pr_info("santos-pmu-preinit: punit apm_sts=0x%08x apm_cmd=0x%08x ospm_sss=0x%08x ospm_ssc=0x%08x\n",
		apm_sts, apm_cmd, ospm_sss, ospm_ssc);
}

static int __init santos_pmu_preinit_init(void)
{
	struct pci_dev *pmu_dev;
	void __iomem *pmu = NULL;
	resource_size_t bar0;
	u16 pci_cmd = 0;
	u32 apm_base, ospm_base;
	unsigned long apm_sts_p, apm_cmd_p, ospm_sss_p, ospm_ssc_p;
	u32 apm_sts, apm_cmd, ospm_sss, ospm_ssc;
	u32 vec[4], cfg[4];
	u32 sts, cmd_rb, sss[4];
	int i, ret = 0, polls = 0;
	bool applied = false;

	pr_info("santos-pmu-preinit: start do_init=%d\n", do_init);

	santos_root = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0, 0));
	if (!santos_root) {
		pr_err("santos-pmu-preinit: no host bridge 00:00.0\n");
		return -ENODEV;
	}

	apm_base = santos_msgbus_read32(SANTOS_PUNIT_PORT, SANTOS_APMBA) & 0xffff;
	ospm_base = santos_msgbus_read32(SANTOS_PUNIT_PORT, SANTOS_OSPMBA) & 0xffff;
	apm_sts_p = apm_base + SANTOS_APM_STS;
	apm_cmd_p = apm_base + SANTOS_APM_CMD;
	ospm_sss_p = ospm_base + SANTOS_OSPM_SSS;
	ospm_ssc_p = ospm_base + SANTOS_OSPM_SSC;
	apm_sts = inl(apm_sts_p);
	apm_cmd = inl(apm_cmd_p);
	ospm_sss = inl(ospm_sss_p);
	ospm_ssc = inl(ospm_ssc_p);
	pr_info("santos-pmu-preinit: punit bases apm=0x%x ospm=0x%x\n",
		apm_base, ospm_base);

	pmu_dev = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(2, 2));
	if (!pmu_dev) {
		pr_err("santos-pmu-preinit: no SCU PMU 00:02.2\n");
		ret = -ENODEV;
		goto out_root;
	}
	if (pmu_dev->vendor != SANTOS_PMU_VENDOR ||
	    pmu_dev->device != SANTOS_PMU_DEVICE) {
		pr_err("santos-pmu-preinit: 00:02.2 identity mismatch [%04x:%04x]\n",
		       pmu_dev->vendor, pmu_dev->device);
		ret = -ENODEV;
		goto out_pmu_dev;
	}
	pci_read_config_word(pmu_dev, PCI_COMMAND, &pci_cmd);
	bar0 = pci_resource_start(pmu_dev, 0);
	pr_info("santos-pmu-preinit: 00:02.2 [%04x:%04x] CMD=0x%04x BAR0=0x%llx\n",
		pmu_dev->vendor, pmu_dev->device, pci_cmd,
		(unsigned long long)bar0);
	if (!(pci_cmd & PCI_COMMAND_MEMORY)) {
		pr_err("santos-pmu-preinit: PMU MEM decode disabled, abort\n");
		ret = -EACCES;
		goto out_pmu_dev;
	}

	pmu = ioremap(bar0, 0x1000);
	if (!pmu) {
		pr_err("santos-pmu-preinit: PMU ioremap failed\n");
		ret = -ENOMEM;
		goto out_pmu_dev;
	}

	santos_pmu_snapshot(pmu, "BEFORE", apm_sts, apm_cmd, ospm_sss, ospm_ssc);

	santos_enumerate_lss();
	santos_compute_vector(vec, cfg);

	if (!do_init) {
		pr_info("santos-pmu-preinit: census only (do_init=0), no writes\n");
		pr_info("santos-pmu-preinit: verdict=CENSUS\n");
		goto out_unmap;
	}

	/* Golden pmu_init() hardware order. */
	iowrite32(WAKE_ENABLE_0, pmu + PM_WKC0);
	iowrite32(WAKE_ENABLE_1, pmu + PM_WKC1);
	{
		u32 ics = ioread32(pmu + PM_ICS);
		iowrite32(ics | PM_ICS_INT_EN, pmu + PM_ICS);
	}
	iowrite32(vec[0], pmu + PM_SSC0);
	iowrite32(vec[1], pmu + PM_SSC1);
	iowrite32(vec[2], pmu + PM_SSC2);
	iowrite32(vec[3], pmu + PM_SSC3);
	iowrite32(INTERACTIVE_VALUE, pmu + PM_CMD);
	applied = true;

	/* Bounded wait for !busy (golden retries ~60 x usleep(10,500)). */
	for (i = 0; i < 500; i++) {
		sts = ioread32(pmu + PM_STS);
		if (!(sts & PM_STS_BUSY))
			break;
		udelay(1000);
		polls++;
	}
	sts = ioread32(pmu + PM_STS);

	apm_sts = inl(apm_sts_p);
	apm_cmd = inl(apm_cmd_p);
	ospm_sss = inl(ospm_sss_p);
	ospm_ssc = inl(ospm_ssc_p);
	santos_pmu_snapshot(pmu, "AFTER", apm_sts, apm_cmd, ospm_sss, ospm_ssc);

	cmd_rb = ioread32(pmu + PM_CMD);
	sss[0] = ioread32(pmu + PM_SSS0);
	sss[1] = ioread32(pmu + PM_SSS1);
	sss[2] = ioread32(pmu + PM_SSS2);
	sss[3] = ioread32(pmu + PM_SSS3);
	{
		bool busy_clear = !(sts & PM_STS_BUSY);
		bool mode1 = ((sts >> 9) & 0xf) == 1;
		bool cmd_ok = cmd_rb == INTERACTIVE_VALUE;
		bool sss_ok = sss[0] == vec[0] && sss[1] == vec[1] &&
			      sss[2] == vec[2] && sss[3] == vec[3];
		bool ok = busy_clear && mode1 && cmd_ok && sss_ok;

		pr_info("santos-pmu-preinit: check busy_clear=%d mode_id1=%d cmd=0x%08x sss_mirrors_ssc=%d polls=%d\n",
			busy_clear, mode1, cmd_rb, sss_ok, polls);
		pr_info("santos-pmu-preinit: verdict=%s\n", ok ? "OK" : "MISMATCH");
	}

out_unmap:
	(void)applied;
	iounmap(pmu);
out_pmu_dev:
	pci_dev_put(pmu_dev);
out_root:
	pci_dev_put(santos_root);
	santos_root = NULL;
	return ret;
}

static void __exit santos_pmu_preinit_exit(void)
{
	pr_info("santos-pmu-preinit: unload (no power-down by design)\n");
}

module_init(santos_pmu_preinit_init);
module_exit(santos_pmu_preinit_exit);

MODULE_AUTHOR("Santos10wifi bring-up");
MODULE_DESCRIPTION("A121 7.2 SCU PMU pre-init reproduction (golden 3.4 pmu_init)");
MODULE_LICENSE("GPL");
