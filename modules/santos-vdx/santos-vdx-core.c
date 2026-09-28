// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include "santos-vdx-compat.h"

void santos_vdx_mmu_selftest_run(void);
int santos_vdx_engine_init(void);
void santos_vdx_engine_fini(void);

static int selftest;
module_param(selftest, int, 0444);
MODULE_PARM_DESC(selftest, "1 = run the MMU page-table selftest at load");

static int engine_test;
module_param(engine_test, int, 0444);
MODULE_PARM_DESC(engine_test, "1 = run the punit engine bring-up at load");

static int __init santos_vdx_core_init(void)
{
	pr_info("santos-vdx: Gate 3 core loading (MMU port), selftest=%d\n",
		selftest);
	if (selftest)
		santos_vdx_mmu_selftest_run();
	if (engine_test) {
		int rc = santos_vdx_engine_init();
		pr_info("santos-vdx: engine_test rc=%d\n", rc);
		if (rc) {
			santos_vdx_engine_fini();
			return rc;
		}
	}
	return 0;
}

static void __exit santos_vdx_core_exit(void)
{
	santos_vdx_engine_fini();
	pr_info("santos-vdx: Gate 3 core unloaded\n");
}

module_init(santos_vdx_core_init);
module_exit(santos_vdx_core_exit);
MODULE_DESCRIPTION("Santos VDX Gate 3 core (MMU)");
MODULE_LICENSE("GPL");
