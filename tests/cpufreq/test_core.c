/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "santos-cpufreq-core.h"

static unsigned int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); \
} } while (0)

struct mock {
	sc_u64 value[4];
	unsigned int reads, writes;
	unsigned int fail_read;
	int write_mode;
};

static int read_ctl(void *ctx, unsigned int cpu, sc_u64 *value)
{
	struct mock *m = ctx;

	CHECK(cpu < 4);
	if (++m->reads == m->fail_read)
		return -EIO;
	*value = m->value[cpu];
	return 0;
}

static int write_ctl(void *ctx, unsigned int cpu, sc_u64 value)
{
	struct mock *m = ctx;

	CHECK(cpu < 4);
	m->writes++;
	if (m->write_mode == 1)
		return -EIO;
	if (m->write_mode == 3)
		return 0;
	m->value[cpu] = m->write_mode == 4 ?
		santos_merge_control(value, 0xbeef) : value;
	return m->write_mode == 2 ? -EIO : 0;
}

static void init(struct mock *m, struct santos_control_state *s)
{
	unsigned int cpu;

	memset(m, 0, sizeof(*m));
	memset(s, 0, sizeof(*s));
	for (cpu = 0; cpu < 4; cpu++)
		m->value[cpu] = 0x1234567800000645ULL;
	s->original = m->value[0];
	s->last = s->attempted = 0x645;
}

int main(void)
{
	struct santos_sfi_state states[] = {
		{1600, 100, 0xc57}, {1333, 100, 0xa4d},
		{933, 100, 0x746}, {800, 100, 0x645},
	};
	struct santos_sfi_state bad[4];
	struct mock m;
	struct santos_control_state s;
	struct santos_control_ops ops = {read_ctl, write_ctl, &m};
	unsigned int i;

	CHECK(santos_validate_states(states, 4) == 0);
	CHECK(santos_validate_states(NULL, 4) == -EINVAL);
	CHECK(santos_validate_states(states, 0) == -EINVAL);
	CHECK(santos_validate_states(states, 3) == -EINVAL);
	CHECK(santos_validate_states(states, 5) == -EINVAL);
	for (i = 0; i < 4; i++) {
		memcpy(bad, states, sizeof(bad)); bad[i].mhz++;
		CHECK(santos_validate_states(bad, 4) == -EINVAL);
		memcpy(bad, states, sizeof(bad)); bad[i].control ^= 1;
		CHECK(santos_validate_states(bad, 4) == -EINVAL);
		memcpy(bad, states, sizeof(bad)); bad[i].latency_us = 0;
		CHECK(santos_validate_states(bad, 4) == -EINVAL);
		bad[i].latency_us = 1000001;
		CHECK(santos_validate_states(bad, 4) == -EINVAL);
	}
	CHECK(santos_merge_control(0x1234567800000645ULL, 0xc57) ==
	      0x1234567800000c57ULL);
	CHECK(!santos_known_control(0x10c57));
	CHECK(santos_restore_control(NULL, &ops, 0) == -EINVAL);

	init(&m, &s); m.value[0] = 0;
	CHECK(santos_verify_untouched(0, &ops, 0) == 0 && !m.writes);
	m.value[0] = 0x645;
	CHECK(santos_verify_untouched(0, &ops, 0) == -EBUSY && !m.writes);
	m.fail_read = m.reads + 1;
	CHECK(santos_verify_untouched(0, &ops, 0) == -EIO && !m.writes);
	CHECK(santos_verify_untouched(0, NULL, 0) == -EINVAL);

	init(&m, &s);
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == 0);
	CHECK(s.changed && !s.uncertain && s.last == 0xc57 && m.writes == 1);
	CHECK(m.value[0] == 0x1234567800000c57ULL && m.value[1] == s.original);
	CHECK(santos_restore_control(&s, &ops, 0) == 0);
	CHECK(!s.changed && !s.uncertain && m.value[0] == s.original);

	init(&m, &s);
	CHECK(santos_apply_control(&s, &ops, 0, 0x645, false) == 0 && !m.writes);
	CHECK(santos_apply_control(&s, &ops, 0, 0x645, true) == 0 && m.writes == 1);
	CHECK(!s.changed);
	CHECK(santos_apply_control(&s, &ops, 0, 0xbeef, false) == -EINVAL);

	init(&m, &s); m.fail_read = 1;
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == -EIO);
	CHECK(!m.writes && !s.changed);

	init(&m, &s); m.write_mode = 1;
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == -EIO);
	CHECK(!s.changed && m.value[0] == s.original);

	init(&m, &s); m.write_mode = 2;
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == -EIO);
	CHECK(s.changed && !s.uncertain && s.last == 0xc57);
	m.write_mode = 0;
	CHECK(santos_restore_control(&s, &ops, 0) == 0 && m.value[0] == s.original);

	init(&m, &s); m.fail_read = 2;
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == -EIO);
	CHECK(s.changed && s.uncertain);
	CHECK(santos_apply_control(&s, &ops, 0, 0xa4d, false) == -EIO);
	CHECK(m.writes == 1);
	CHECK(santos_restore_control(&s, &ops, 0) == 0);
	CHECK(!s.changed && !s.uncertain && m.value[0] == s.original);

	init(&m, &s); m.write_mode = 3;
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == -EIO);
	CHECK(!s.changed && !s.uncertain && m.value[0] == s.original);

	init(&m, &s); m.write_mode = 4;
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == -EIO);
	CHECK(s.changed && s.uncertain);
	CHECK(santos_restore_control(&s, &ops, 0) == -EBUSY && m.writes == 1);
	CHECK((m.value[0] & SANTOS_CONTROL_MASK) == 0xbeef);

	init(&m, &s); m.value[0] = santos_merge_control(m.value[0], 0xa4d);
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == -EBUSY);
	CHECK(santos_restore_control(&s, &ops, 0) == -EBUSY && !m.writes);

	init(&m, &s); m.value[0] ^= 1ULL << 40;
	CHECK(santos_apply_control(&s, &ops, 0, 0xc57, false) == 0);
	CHECK(santos_restore_control(&s, &ops, 0) == 0);
	CHECK(m.value[0] == (s.original ^ (1ULL << 40)));

	printf("CORE_CHECKS=%u PASS; table guards, ownership, partial/error writes, "
	       "uncertain readback, reserved bits and restoration tested\n", checks);
	return 0;
}
