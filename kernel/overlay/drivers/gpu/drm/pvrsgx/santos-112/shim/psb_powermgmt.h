// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S3 skeleton shim for PSB power/IRQ interop.
 *
 * sysconfig.c calls OSPM island power + psb_irq_uninstall_islands
 * from the old Intel media stack (SCU IPC islands, PSB IRQ router),
 * neither of which exists in 7.2. Signatures copied from
 * drivers/staging/intel_media/common/psb_powermgmt.h and
 * display/pnw/drv/psb_irq.h; island bits are placeholders.
 * Real island control lands in S5 (P2 prerequisite per A104 verdict).
 * Skeleton links, never runs.
 */
#ifndef SANTOS_SHIM_PSB_POWERMGMT_H
#define SANTOS_SHIM_PSB_POWERMGMT_H

struct drm_device;

#define OSPM_GRAPHICS_ISLAND	0x01
#define OSPM_VIDEO_DEC_ISLAND	0x02
#define OSPM_VIDEO_ENC_ISLAND	0x04
#define OSPM_GL3_CACHE_ISLAND	0x08
#define OSPM_DISPLAY_ISLAND	0x40

typedef enum _UHBUsage {
	OSPM_UHB_ONLY_IF_ON = 0,
	OSPM_UHB_FORCE_POWER_ON,
} UHBUsage;

bool ospm_power_using_hw_begin(int hw_island, UHBUsage usage);
void ospm_power_using_hw_end(int hw_island);
bool ospm_power_is_hw_on(int hw_islands);
void ospm_power_island_down(int hw_islands);
int ospm_power_island_up(int hw_islands);
void ospm_power_graphics_island_down(int hw_islands);
void ospm_power_graphics_island_up(int hw_islands);
void psb_irq_uninstall_islands(struct drm_device *dev, int hw_islands);

#endif /* SANTOS_SHIM_PSB_POWERMGMT_H */
