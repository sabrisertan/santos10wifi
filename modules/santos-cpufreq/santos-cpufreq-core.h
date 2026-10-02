/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SANTOS_CPUFREQ_CORE_H
#define SANTOS_CPUFREQ_CORE_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
typedef u32 sc_u32;
typedef u64 sc_u64;
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t sc_u32;
typedef uint64_t sc_u64;
#endif

#define SANTOS_STATE_COUNT 4
#define SANTOS_CPU_COUNT 4
#define SANTOS_CONTROL_MASK 0xffffULL

struct santos_sfi_state {
	sc_u32 mhz;
	sc_u32 latency_us;
	sc_u32 control;
};

struct santos_control_state {
	sc_u64 original;
	sc_u32 last;
	sc_u32 attempted;
	bool changed;
	bool uncertain;
};

struct santos_control_ops {
	int (*read)(void *ctx, unsigned int cpu, sc_u64 *value);
	int (*write)(void *ctx, unsigned int cpu, sc_u64 value);
	void *ctx;
};

static inline int santos_verify_untouched(sc_u64 original,
					const struct santos_control_ops *ops,
					unsigned int cpu)
{
	sc_u64 value;
	int ret;

	if (!ops || !ops->read)
		return -EINVAL;
	ret = ops->read(ops->ctx, cpu, &value);
	if (ret)
		return ret;
	return (value & SANTOS_CONTROL_MASK) ==
	       (original & SANTOS_CONTROL_MASK) ? 0 : -EBUSY;
}

static inline bool santos_known_control(sc_u32 control)
{
	return control == 0xc57 || control == 0xa4d ||
	       control == 0x746 || control == 0x645;
}

/* Narrow first candidate: only the verified GT-P5210 firmware state set. */
static inline int santos_validate_states(const struct santos_sfi_state *s,
					unsigned int count)
{
	static const sc_u32 mhz[] = { 1600, 1333, 933, 800 };
	static const sc_u32 ctl[] = { 0xc57, 0xa4d, 0x746, 0x645 };
	unsigned int i;

	if (!s || count != SANTOS_STATE_COUNT)
		return -EINVAL;
	for (i = 0; i < count; i++)
		if (s[i].mhz != mhz[i] || s[i].control != ctl[i] ||
		    !s[i].latency_us || s[i].latency_us > 1000000)
			return -EINVAL;
	return 0;
}

static inline sc_u64 santos_merge_control(sc_u64 register_value, sc_u32 control)
{
	return (register_value & ~SANTOS_CONTROL_MASK) | control;
}

/* A safe-MSR error does not grant permission to overwrite an unknown owner. */
static inline int santos_apply_control(struct santos_control_state *s,
				      const struct santos_control_ops *ops,
				      unsigned int cpu, sc_u32 control, bool force)
{
	sc_u64 before, after;
	int ret, read_ret;

	if (!s || !ops || !ops->read || !ops->write ||
	    !santos_known_control(control))
		return -EINVAL;
	if (s->uncertain)
		return -EIO;
	ret = ops->read(ops->ctx, cpu, &before);
	if (ret)
		return ret;
	if ((before & SANTOS_CONTROL_MASK) != s->last)
		return -EBUSY;
	if (s->last == control && !force)
		return 0;
	s->attempted = control;
	ret = ops->write(ops->ctx, cpu, santos_merge_control(before, control));
	read_ret = ops->read(ops->ctx, cpu, &after);
	if (read_ret) {
		s->uncertain = true;
		s->changed = true;
		return ret ? ret : read_ret;
	}
	if ((after & SANTOS_CONTROL_MASK) == control) {
		s->last = control;
		s->changed = control != (s->original & SANTOS_CONTROL_MASK);
		s->uncertain = false;
		return ret;
	}
	if ((after & SANTOS_CONTROL_MASK) != (before & SANTOS_CONTROL_MASK)) {
		s->uncertain = true;
		s->changed = true;
	}
	return ret ? ret : -EIO;
}

/* All requests feed one measured package clock. Roll a failed partial change
 * back to the previous request set, preserving ownership and reserved bits. */
static inline int santos_apply_domain(struct santos_control_state *states,
				     const struct santos_control_ops *ops,
				     unsigned int writable_mask,
				     sc_u32 control, bool force)
{
	sc_u32 previous[SANTOS_CPU_COUNT];
	unsigned int cpu;
	int ret = 0;

	if (!states || !ops || !ops->read || !ops->write ||
	    !writable_mask || (writable_mask & ~((1U << SANTOS_CPU_COUNT) - 1)) ||
	    !santos_known_control(control))
		return -EINVAL;
	for (cpu = 0; cpu < SANTOS_CPU_COUNT; cpu++) {
		sc_u64 value;

		previous[cpu] = states[cpu].last;
		if (!(writable_mask & (1U << cpu)))
			continue;
		if (states[cpu].uncertain || !santos_known_control(previous[cpu]))
			return -EIO;
		ret = ops->read(ops->ctx, cpu, &value);
		if (ret)
			return ret;
		if ((value & SANTOS_CONTROL_MASK) != previous[cpu])
			return -EBUSY;
	}
	for (cpu = 0; cpu < SANTOS_CPU_COUNT; cpu++) {
		if (!(writable_mask & (1U << cpu)))
			continue;
		ret = santos_apply_control(&states[cpu], ops, cpu, control, force);
		if (ret)
			break;
	}
	if (ret) {
		unsigned int rollback_cpu;

		for (rollback_cpu = 0; rollback_cpu <= cpu; rollback_cpu++) {
			if (!(writable_mask & (1U << rollback_cpu)) ||
			    states[rollback_cpu].uncertain ||
			    states[rollback_cpu].last == previous[rollback_cpu])
				continue;
			/* The error remains sticky at the driver: uncertain or failed
			 * rollback requires shutdown/readback, never another target. */
			(void)santos_apply_control(&states[rollback_cpu], ops,
						   rollback_cpu,
						   previous[rollback_cpu], false);
		}
	}
	return ret;
}

static inline int santos_restore_control(struct santos_control_state *s,
					const struct santos_control_ops *ops,
					unsigned int cpu)
{
	sc_u64 before, after;
	sc_u32 original;
	sc_u32 live_control;
	int ret;

	if (!s || !ops || !ops->read || !ops->write)
		return -EINVAL;
	original = s->original & SANTOS_CONTROL_MASK;
	if (!santos_known_control(original))
		return -EINVAL;
	ret = ops->read(ops->ctx, cpu, &before);
	if (ret)
		return ret;
	live_control = before & SANTOS_CONTROL_MASK;
	if (live_control == original)
		goto restored;
	if (!s->changed || (live_control != s->last &&
	    !(s->uncertain && live_control == s->attempted)))
		return -EBUSY;
	ret = ops->write(ops->ctx, cpu, santos_merge_control(before, original));
	if (ret)
		return ret;
	ret = ops->read(ops->ctx, cpu, &after);
	if (ret)
		return ret;
	if ((after & SANTOS_CONTROL_MASK) != original)
		return -EIO;
restored:
	s->last = original;
	s->attempted = original;
	s->changed = false;
	s->uncertain = false;
	return 0;
}

#endif
