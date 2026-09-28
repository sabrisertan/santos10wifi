/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SANTOS_IRQ_H
#define SANTOS_IRQ_H

#include <linux/io.h>
struct drm_device;

/* Services owns the BAR mapping and callback; shell owns the MSI vector.
 * Start after MISR installation, stop before MISR/device/BAR teardown.
 * The provider depends on the shell and must stop before its code unloads.
 */
int santos_pvr_irq_start(void __iomem *irq_regs,
			 void __iomem *display_regs,
			 int (*service)(struct drm_device *));
void santos_pvr_irq_stop(void);

#endif
