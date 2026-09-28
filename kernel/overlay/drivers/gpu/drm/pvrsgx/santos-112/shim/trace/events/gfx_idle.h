// SPDX-License-Identifier: GPL-2.0-only
/*
 * Santos S3 skeleton shim for trace/events/gfx_idle.h.
 *
 * sgxutils.c calls trace_gfx_idle_{entry,exit,poweroff} (PSB GFX
 * idle tracepoints). No-ops for the no-hardware skeleton; S5 wires
 * real tracepoints if wanted.
 */
#ifndef SANTOS_SHIM_GFX_IDLE_H
#define SANTOS_SHIM_GFX_IDLE_H

#define trace_gfx_idle_entry(cpu) do { (void)(cpu); } while (0)
#define trace_gfx_idle_exit(cpu) do { (void)(cpu); } while (0)
#define trace_gfx_idle_poweroff(cpu) do { (void)(cpu); } while (0)

#endif /* SANTOS_SHIM_GFX_IDLE_H */
