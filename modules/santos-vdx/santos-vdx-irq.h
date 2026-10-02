/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SANTOS_VDX_IRQ_H
#define SANTOS_VDX_IRQ_H

/* Share the shell's already-installed SGX MSI vector; never allocate a second
 * vector or replace Services' callback. Register only after engine queue/MMIO
 * setup and unregister before its code/MMIO can be released. Registration pins
 * the provider owning the shared VDC BAR. The callback must not sleep or acquire
 * the shell IRQ/BO mutexes. The stopped callback runs after IRQ synchronization
 * and must not access the shared BAR or acquire those mutexes; it switches the
 * engine to recovery polling if Services explicitly detaches the vector.
 * Identity-pinned unregister synchronizes the IRQ. */
int santos_pvr_vdx_irq_register(void (*service)(void), void (*stopped)(void));
void santos_pvr_vdx_irq_unregister(void (*service)(void));

#endif
