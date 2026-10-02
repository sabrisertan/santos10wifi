/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "santos-cpufreq-core.h"

static unsigned int checks;
#define CHECK(c) do { checks++; if (!(c)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); \
} } while (0)

struct domain_mock {
	sc_u64 value[4];
	unsigned int reads[4], writes[4];
	int fail_cpu, failure_mode;
	bool failed, readback_error;
	bool fail_rollback;
};

static int read_ctl(void *ctx, unsigned int cpu, sc_u64 *value)
{
	struct domain_mock *m = ctx;

	CHECK(cpu < 4);
	m->reads[cpu]++;
	if (m->readback_error && (int)cpu == m->fail_cpu) {
		m->readback_error = false;
		return -EIO;
	}
	*value = m->value[cpu];
	return 0;
}

static int write_ctl(void *ctx, unsigned int cpu, sc_u64 value)
{
	struct domain_mock *m = ctx;

	CHECK(cpu < 4);
	m->writes[cpu]++;
	if (m->fail_rollback && cpu == 1 && m->writes[cpu] == 2)
		return -EIO;
	if ((int)cpu == m->fail_cpu && !m->failed) {
		m->failed = true;
		if (m->failure_mode == 0)
			return -EIO;
		if (m->failure_mode == 2) {
			m->value[cpu] = santos_merge_control(value, 0xbeef);
			return 0;
		}
		m->value[cpu] = value;
		if (m->failure_mode == 3) {
			m->readback_error = true;
			return 0;
		}
		return -EIO;
	}
	m->value[cpu] = value;
	return 0;
}

static void init(struct domain_mock *m, struct santos_control_state *s)
{
	unsigned int cpu;

	memset(m, 0, sizeof(*m));
	memset(s, 0, sizeof(*s) * 4);
	m->fail_cpu = -1;
	for (cpu = 1; cpu < 4; cpu++) {
		m->value[cpu] = ((sc_u64)cpu << 40) | 0x645;
		s[cpu].original = m->value[cpu];
		s[cpu].last = s[cpu].attempted = 0x645;
	}
}

static void all_controls(struct domain_mock *m, sc_u32 expected)
{
	unsigned int cpu;

	CHECK(m->value[0] == 0 && m->writes[0] == 0);
	for (cpu = 1; cpu < 4; cpu++)
		CHECK(m->value[cpu] == (((sc_u64)cpu << 40) | expected));
}

int main(void)
{
	struct domain_mock m;
	struct santos_control_state s[4];
	struct santos_control_ops ops = {read_ctl, write_ctl, &m};
	unsigned int cpu;

	init(&m, s);
	CHECK(santos_apply_domain(s, &ops, 14, 0xc57, false) == 0);
	all_controls(&m, 0xc57);
	for (cpu = 1; cpu < 4; cpu++) {
		CHECK(s[cpu].changed && !s[cpu].uncertain && m.writes[cpu] == 1);
		CHECK(santos_restore_control(&s[cpu], &ops, cpu) == 0);
	}
	all_controls(&m, 0x645);

	init(&m, s);
	CHECK(santos_apply_domain(s, &ops, 14, 0x645, false) == 0);
	CHECK(m.writes[1] == 0 && m.writes[2] == 0 && m.writes[3] == 0);
	CHECK(santos_apply_domain(s, &ops, 14, 0x645, true) == 0);
	CHECK(m.writes[1] == 1 && m.writes[2] == 1 && m.writes[3] == 1);
	CHECK(santos_apply_domain(s, &ops, 0, 0xc57, false) == -EINVAL);
	CHECK(santos_apply_domain(s, &ops, 31, 0xc57, false) == -EINVAL);
	CHECK(santos_apply_domain(s, &ops, 14, 0xbeef, false) == -EINVAL);
	CHECK(santos_apply_domain(NULL, &ops, 14, 0xc57, false) == -EINVAL);
	CHECK(santos_apply_domain(s, NULL, 14, 0xc57, false) == -EINVAL);
	CHECK(santos_apply_domain(s, &ops, 15, 0xc57, false) == -EIO);
	all_controls(&m, 0x645);

	init(&m, s); m.value[2] = santos_merge_control(m.value[2], 0xa4d);
	CHECK(santos_apply_domain(s, &ops, 14, 0xc57, false) == -EBUSY);
	CHECK(m.writes[1] == 0 && m.writes[2] == 0 && m.writes[3] == 0);

	for (int mode = 0; mode <= 1; mode++) {
		init(&m, s); m.fail_cpu = 2; m.failure_mode = mode;
		CHECK(santos_apply_domain(s, &ops, 14, 0xc57, false) == -EIO);
		all_controls(&m, 0x645);
		CHECK(!s[1].changed && !s[2].changed && !s[3].changed);
	}

	init(&m, s);
	CHECK(santos_apply_domain(s, &ops, 14, 0xa4d, false) == 0);
	m.fail_cpu = 2;
	CHECK(santos_apply_domain(s, &ops, 14, 0xc57, false) == -EIO);
	all_controls(&m, 0xa4d); /* rollback prior request, not always boot800MHz */
	for (cpu = 1; cpu < 4; cpu++)
		CHECK(santos_restore_control(&s[cpu], &ops, cpu) == 0);
	all_controls(&m, 0x645);

	init(&m, s); m.fail_cpu = 2; m.failure_mode = 2;
	CHECK(santos_apply_domain(s, &ops, 14, 0xc57, false) == -EIO);
	CHECK(s[2].uncertain && (m.value[2] & SANTOS_CONTROL_MASK) == 0xbeef);
	CHECK(santos_restore_control(&s[2], &ops, 2) == -EBUSY);
	CHECK(m.writes[2] == 1); /* never overwrite unknown owner */
	CHECK((m.value[1] & SANTOS_CONTROL_MASK) == 0x645);

	init(&m, s); m.fail_cpu = 2; m.failure_mode = 3;
	CHECK(santos_apply_domain(s, &ops, 14, 0xc57, false) == -EIO);
	CHECK(s[2].uncertain && (m.value[2] & SANTOS_CONTROL_MASK) == 0xc57);
	CHECK(santos_apply_domain(s, &ops, 14, 0xa4d, false) == -EIO);
	CHECK(santos_restore_control(&s[2], &ops, 2) == 0);
	all_controls(&m, 0x645);

	init(&m, s); m.fail_cpu = 2; m.fail_rollback = true;
	CHECK(santos_apply_domain(s, &ops, 14, 0xc57, false) == -EIO);
	CHECK(s[1].changed && s[1].last == 0xc57);
	CHECK(santos_restore_control(&s[1], &ops, 1) == 0);
	all_controls(&m, 0x645);
	printf("DOMAIN_CHECKS=%u PASS; common package requests, neutralCPU0, "
	       "ownership, partial/error/ambiguous writes and rollback\n", checks);
	return 0;
}
