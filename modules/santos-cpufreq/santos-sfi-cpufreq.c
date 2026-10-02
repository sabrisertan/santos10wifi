// SPDX-License-Identifier: GPL-2.0-only
/*
 * Leased GT-P5210/Cloverview package CPUFreq driver.
 * Hardware oracle: Intel/Samsung 3.4 drivers/cpufreq/sfi-cpufreq.c.
 * Default observe-only; no MISC/EIST/thermal writes, no invented P-states.
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/sfi.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <asm/msr.h>
#include <asm/msr-index.h>
#include <asm/processor.h>
#include <asm/tsc.h>
#include "santos-cpufreq-core.h"

static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "false=read-only observer; true=leased package CPUFreq driver");
static unsigned int lease_ms = 90000;
module_param(lease_ms, uint, 0444);
MODULE_PARM_DESC(lease_ms, "active control lease, 2000..120000 ms; expiry restores original control");
static char *owner_token = "";
module_param(owner_token, charp, 0444);
MODULE_PARM_DESC(owner_token, "optional immutable manager instance token (lowercase hex/hyphen, max64)");
static bool registered, expired, stopping, released, restored;
static int last_error, restore_error;
static unsigned int writes, reference_khz;

static DEFINE_MUTEX(control_lock);
static DEFINE_MUTEX(lifecycle_lock);
static struct santos_sfi_state states[SANTOS_STATE_COUNT];
static struct cpufreq_frequency_table table[SANTOS_STATE_COUNT + 1];
static struct santos_control_state controls[SANTOS_CPU_COUNT];
static bool first_request;
static bool readonly_cpu[SANTOS_CPU_COUNT];
static unsigned int writable_mask;
static u64 initial_status[SANTOS_CPU_COUNT], initial_misc[SANTOS_CPU_COUNT];
static u32 firmware_apic[SANTOS_CPU_COUNT];
static bool freq_seen, cpus_seen, captured;
static unsigned long deadline;
static void lease_expired(struct work_struct *work);
static DECLARE_DELAYED_WORK(lease_work, lease_expired);

static int ctl_read(void *ctx, unsigned int cpu, sc_u64 *value)
{
	(void)ctx;
	return rdmsrq_safe_on_cpu(cpu, MSR_IA32_PERF_CTL, value);
}

static int ctl_write(void *ctx, unsigned int cpu, sc_u64 value)
{
	int ret;

	(void)ctx;
	ret = wrmsrq_safe_on_cpu(cpu, MSR_IA32_PERF_CTL, value);
	if (!ret)
		writes++;
	return ret;
}

static const struct santos_control_ops control_ops = {
	.read = ctl_read, .write = ctl_write,
};

static bool valid_sfi_header(struct sfi_table_header *h, unsigned int bytes)
{
	const u8 *raw = (const u8 *)h;
	u8 sum = 0;
	unsigned int i;

	if (h->len != sizeof(*h) + bytes)
		return false;
	for (i = 0; i < h->len; i++)
		sum += raw[i];
	return !sum;
}

static int __init parse_freq(struct sfi_table_header *h)
{
	const u8 *raw = (const u8 *)(h + 1);
	unsigned int i;

	if (freq_seen || !valid_sfi_header(h, SANTOS_STATE_COUNT * 12))
		return -EINVAL;
	for (i = 0; i < SANTOS_STATE_COUNT; i++, raw += 12) {
		states[i].mhz = get_unaligned_le32(raw);
		states[i].latency_us = get_unaligned_le32(raw + 4);
		states[i].control = get_unaligned_le32(raw + 8);
	}
	if (santos_validate_states(states, SANTOS_STATE_COUNT))
		return -EINVAL;
	freq_seen = true;
	return 0;
}

static int __init parse_cpus(struct sfi_table_header *h)
{
	const u8 *raw = (const u8 *)(h + 1);
	unsigned int i, seen = 0;

	if (cpus_seen || !valid_sfi_header(h, SANTOS_CPU_COUNT * 4))
		return -EINVAL;
	for (i = 0; i < SANTOS_CPU_COUNT; i++, raw += 4) {
		u32 apic = get_unaligned_le32(raw);

		if (apic >= SANTOS_CPU_COUNT || (seen & BIT(apic)))
			return -EINVAL;
		seen |= BIT(apic);
		firmware_apic[i] = apic;
	}
	cpus_seen = true;
	return 0;
}

static unsigned int status_frequency(u64 status)
{
	unsigned int i, ratio = (status >> 8) & 0xff;

	for (i = 0; i < SANTOS_STATE_COUNT; i++)
		if (((states[i].control >> 8) & 0xff) == ratio)
			return states[i].mhz * 1000;
	return 0;
}

/* A missing/invalid DTS is not permission for a high-frequency experiment. */
static int thermal_gate(unsigned int cpu)
{
	u64 thermal;
	int ret = rdmsrq_safe_on_cpu(cpu, MSR_IA32_THERM_STATUS, &thermal);

	if (ret)
		return ret;
	if (!(thermal & BIT_ULL(31)) ||
	    (thermal & (THERM_STATUS_PROCHOT | THERM_STATUS_POWER_LIMIT)) ||
	    ((thermal >> 16) & 0x7f) < 15)
		return -EAGAIN;
	return 0;
}

static unsigned int santos_get(unsigned int cpu)
{
	u64 status;

	if (cpu >= SANTOS_CPU_COUNT || !captured ||
	    rdmsrq_safe_on_cpu(cpu, MSR_IA32_PERF_STATUS, &status))
		return 0;
	return status_frequency(status);
}

static int santos_target(struct cpufreq_policy *policy, unsigned int index)
{
	unsigned int cpu;
	unsigned int raw_index;
	int ret;

	if (policy->cpu >= SANTOS_CPU_COUNT || index >= SANTOS_STATE_COUNT)
		return -EINVAL;
	raw_index = table[index].driver_data;
	if (raw_index >= SANTOS_STATE_COUNT)
		return -EINVAL;
	mutex_lock(&control_lock);
	if (stopping || expired || time_after_eq(jiffies, deadline)) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (last_error) {
		ret = last_error;
		goto out;
	}
	if (states[raw_index].mhz > 800) {
		for_each_cpu(cpu, policy->cpus) {
			ret = thermal_gate(cpu);
			if (ret)
				goto out;
		}
	}
	ret = santos_apply_domain(controls, &control_ops, writable_mask,
				  states[raw_index].control, first_request);
	if (!ret)
		first_request = false;
	if (ret)
		last_error = ret;
out:
	mutex_unlock(&control_lock);
	return ret;
}

static int santos_policy_init(struct cpufreq_policy *policy)
{
	unsigned int i, latency = 0;

	if (!captured || policy->cpu >= SANTOS_CPU_COUNT)
		return -ENODEV;
	for (i = 0; i < SANTOS_STATE_COUNT; i++)
		latency = max(latency, states[i].latency_us * 1000);
	policy->freq_table = table;
	policy->cpuinfo.transition_latency = latency;
	/* Real two-core probes show that any logical-CPU request controls the
	 * whole package at the highest request. CPU0 therefore joins the policy
	 * even when its firmware request register is zero/read-only.
	 */
	policy->shared_type = CPUFREQ_SHARED_TYPE_ALL;
	cpumask_copy(policy->cpus, cpu_possible_mask);
	mutex_lock(&control_lock);
	first_request = true;
	mutex_unlock(&control_lock);
	return 0;
}

static struct cpufreq_driver santos_driver = {
	.name = "santos-sfi",
	.flags = CPUFREQ_CONST_LOOPS,
	.verify = cpufreq_generic_frequency_table_verify,
	.init = santos_policy_init,
	.get = santos_get,
	.target_index = santos_target,
	/* The 7.2 core creates frequency-table attributes automatically. */
};

/* Caller holds control_lock and the CPU-hotplug read lock. */
static int restore_all_locked(void)
{
	unsigned int cpu;

	if (!captured)
		return -ENODEV;
	restore_error = 0;
	for (cpu = 0; cpu < SANTOS_CPU_COUNT; cpu++) {
		int ret;

		if (!cpu_online(cpu))
			ret = -ENODEV;
		else if (readonly_cpu[cpu])
			ret = santos_verify_untouched(controls[cpu].original,
						     &control_ops, cpu);
		else
			ret = santos_restore_control(&controls[cpu], &control_ops,
						     cpu);

		if (ret) {
			restore_error = ret;
			pr_err("SANTOS_SFI_RESTORE cpu=%u error=%d\n", cpu, ret);
		} else {
			pr_info("SANTOS_SFI_RESTORE cpu=%u ok control=0x%x\n",
				cpu, controls[cpu].last);
		}
	}
	restored = !restore_error;
	return restore_error;
}

/* Serialize all teardown without holding control_lock across governor drain.
 * A failed restore keeps the module present and read-only for investigation. */
static int santos_shutdown(bool lease_timeout)
{
	int ret = 0;

	mutex_lock(&lifecycle_lock);
	mutex_lock(&control_lock);
	stopping = true;
	expired |= lease_timeout;
	mutex_unlock(&control_lock);
	if (registered) {
		cpufreq_unregister_driver(&santos_driver);
		registered = false;
	}
	if (enable && captured) {
		cpus_read_lock();
		mutex_lock(&control_lock);
		ret = restore_all_locked();
		released = !ret;
		mutex_unlock(&control_lock);
		cpus_read_unlock();
	}
	mutex_unlock(&lifecycle_lock);
	return ret;
}

static void lease_expired(struct work_struct *work)
{
	(void)work;
	santos_shutdown(true);
	pr_warn("SANTOS_SFI_LEASE_EXPIRED restore_error=%d\n", restore_error);
}

static int keepalive_set(const char *value, const struct kernel_param *kp)
{
	unsigned int request;
	int ret;

	(void)kp;
	if (*owner_token) {
		if (!sysfs_streq(value, owner_token))
			return -EPERM;
	} else {
		ret = kstrtouint(value, 10, &request);
		if (ret || request != 1)
			return -EINVAL;
	}
	mutex_lock(&control_lock);
	if (!enable || !registered || stopping || expired ||
	    time_after_eq(jiffies, deadline) || last_error) {
		ret = -ESHUTDOWN;
	} else {
		deadline = jiffies + msecs_to_jiffies(lease_ms);
		mod_delayed_work(system_wq, &lease_work, msecs_to_jiffies(lease_ms));
		ret = 0;
	}
	mutex_unlock(&control_lock);
	return ret;
}

static int release_set(const char *value, const struct kernel_param *kp)
{
	unsigned int request;
	int ret;

	(void)kp;
	if (*owner_token) {
		if (!sysfs_streq(value, owner_token))
			return -EPERM;
	} else {
		ret = kstrtouint(value, 10, &request);
		if (ret || request != 1)
			return -EINVAL;
	}
	if (!captured || !enable)
		return -EINVAL;
	cancel_delayed_work_sync(&lease_work);
	return santos_shutdown(false);
}

static const struct kernel_param_ops keepalive_ops = { .set = keepalive_set };
static const struct kernel_param_ops release_ops = { .set = release_set };
module_param_cb(keepalive, &keepalive_ops, NULL, 0200);
module_param_cb(release, &release_ops, NULL, 0200);

static int snapshot_get(char *buf, const struct kernel_param *kp)
{
	unsigned int cpu;
	int off = 0;

	(void)kp;
	cpus_read_lock();
	mutex_lock(&control_lock);
	if (!captured) {
		off = scnprintf(buf, PAGE_SIZE, "{\"captured\":false}\n");
		goto out;
	}
	off += scnprintf(buf + off, PAGE_SIZE - off,
		"{\"enable\":%u,\"registered\":%u,\"expired\":%u,"
		"\"released\":%u,\"restored\":%u,\"domain\":\"package\","
		"\"writable_mask\":%u,\"writes\":%u,\"last_error\":%d,\"restore_error\":%d,"
		"\"reference_khz\":%u,\"cpus\":[", enable, registered,
		expired, released, restored, writable_mask, writes, last_error,
		restore_error, reference_khz);
	for (cpu = 0; cpu < SANTOS_CPU_COUNT; cpu++) {
		u64 ctl = 0, status = 0, misc = 0, thermal = 0;
		u64 aperf = 0, mperf = 0;
		int a, b, c, d, e, f;

		a = rdmsrq_safe_on_cpu(cpu, MSR_IA32_PERF_CTL, &ctl);
		b = rdmsrq_safe_on_cpu(cpu, MSR_IA32_PERF_STATUS, &status);
		c = rdmsrq_safe_on_cpu(cpu, MSR_IA32_MISC_ENABLE, &misc);
		d = rdmsrq_safe_on_cpu(cpu, MSR_IA32_THERM_STATUS, &thermal);
		e = rdmsrq_safe_on_cpu(cpu, MSR_IA32_APERF, &aperf);
		f = rdmsrq_safe_on_cpu(cpu, MSR_IA32_MPERF, &mperf);
		off += scnprintf(buf + off, PAGE_SIZE - off,
			"%s{\"cpu\":%u,\"apic\":%u,\"core\":%u,\"writable\":%u,"
			"\"original_ctl\":\"0x%llx\",\"initial_status\":\"0x%llx\","
			"\"initial_misc\":\"0x%llx\",\"ctl\":\"0x%llx\","
			"\"status\":\"0x%llx\",\"misc\":\"0x%llx\","
			"\"thermal\":\"0x%llx\",\"aperf\":\"0x%llx\","
			"\"mperf\":\"0x%llx\",\"errors\":[%d,%d,%d,%d,%d,%d]}",
			cpu ? "," : "", cpu, cpu_data(cpu).topo.initial_apicid,
			cpu_data(cpu).topo.core_id, !readonly_cpu[cpu],
			controls[cpu].original,
			initial_status[cpu], initial_misc[cpu], ctl, status, misc,
			thermal, aperf, mperf, a, b, c, d, e, f);
	}
	off += scnprintf(buf + off, PAGE_SIZE - off, "]}\n");
out:
	mutex_unlock(&control_lock);
	cpus_read_unlock();
	return off;
}

static const struct kernel_param_ops snapshot_ops = { .get = snapshot_get };
module_param_cb(snapshot, &snapshot_ops, NULL, 0444);

static int __init santos_init(void)
{
	unsigned int cpu, i, seen = 0;
	int ret;

	if (!owner_token)
		return -EINVAL;
	for (i = 0; owner_token[i]; i++) {
		char ch = owner_token[i];

		if (i >= 64 || !(ch == '-' || (ch >= '0' && ch <= '9') ||
		    (ch >= 'a' && ch <= 'f')))
			return -EINVAL;
	}
	if (lease_ms < 2000 || lease_ms > 120000 ||
	    boot_cpu_data.x86_vendor != X86_VENDOR_INTEL ||
	    boot_cpu_data.x86 != 6 || boot_cpu_data.x86_model != 0x35)
		return -ENODEV;
	ret = sfi_table_parse(SFI_SIG_FREQ, NULL, NULL, parse_freq);
	if (ret || !freq_seen)
		return ret ? ret : -ENODEV;
	ret = sfi_table_parse(SFI_SIG_CPUS, NULL, NULL, parse_cpus);
	if (ret || !cpus_seen)
		return ret ? ret : -ENODEV;
	for (i = 0; i < SANTOS_STATE_COUNT; i++) {
		table[i].frequency = states[i].mhz * 1000;
		table[i].driver_data = i;
	}
	table[SANTOS_STATE_COUNT].frequency = CPUFREQ_TABLE_END;
	reference_khz = cpu_khz;
	cpus_read_lock();
	if (num_online_cpus() != SANTOS_CPU_COUNT ||
	    num_possible_cpus() != SANTOS_CPU_COUNT) {
		ret = -ENODEV;
		goto unlock;
	}
	for_each_online_cpu(cpu) {
		u32 apic = cpu_data(cpu).topo.initial_apicid;
		bool found = false;

		if (cpu >= SANTOS_CPU_COUNT || apic >= SANTOS_CPU_COUNT ||
		    (seen & BIT(apic)) || cpu_data(cpu).x86 != 6 ||
		    cpu_data(cpu).x86_model != 0x35 ||
		    cpu_data(cpu).topo.pkg_id != cpu_data(0).topo.pkg_id) {
			ret = -ENODEV;
			goto unlock;
		}
		for (i = 0; i < SANTOS_CPU_COUNT; i++)
			found |= firmware_apic[i] == apic;
		if (!found) {
			ret = -ENODEV;
			goto unlock;
		}
		seen |= BIT(apic);
		ret = ctl_read(NULL, cpu, &controls[cpu].original);
		if (!ret)
			ret = rdmsrq_safe_on_cpu(cpu, MSR_IA32_PERF_STATUS,
					       &initial_status[cpu]);
		if (!ret)
			ret = rdmsrq_safe_on_cpu(cpu, MSR_IA32_MISC_ENABLE,
					       &initial_misc[cpu]);
		if (ret)
			goto unlock;
		controls[cpu].last = controls[cpu].original & SANTOS_CONTROL_MASK;
		controls[cpu].attempted = controls[cpu].last;
		/* Observed boot CPU has no PERF_CTL request. Preserve zero
		 * literally by never writing it; do not invent a zero P-state.
		 * Only already-programmed requests receive writes; every CPU is
		 * represented in the measured package policy.
		 */
		readonly_cpu[cpu] = cpu == 0 && controls[cpu].last == 0;
		if (!readonly_cpu[cpu])
			writable_mask |= BIT(cpu);
		if (enable && ((!readonly_cpu[cpu] &&
		    !santos_known_control(controls[cpu].last)) ||
		    !(initial_misc[cpu] & MSR_IA32_MISC_ENABLE_ENHANCED_SPEEDSTEP) ||
		    !status_frequency(initial_status[cpu]) || thermal_gate(cpu))) {
			ret = -EPERM;
			goto unlock;
		}
	}
	captured = true;
	ret = 0;
unlock:
	cpus_read_unlock();
	if (ret)
		return ret;
	if (!enable) {
		pr_info("SANTOS_SFI_OBSERVER_READY writes=0 cpus=4 reference_khz=%u\n",
			reference_khz);
		return 0;
	}
	deadline = jiffies + msecs_to_jiffies(lease_ms);
	ret = cpufreq_register_driver(&santos_driver);
	if (ret) {
		cpus_read_lock();
		mutex_lock(&control_lock);
		restore_all_locked();
		mutex_unlock(&control_lock);
		cpus_read_unlock();
		return ret;
	}
	registered = true;
	schedule_delayed_work(&lease_work, msecs_to_jiffies(lease_ms));
	pr_info("SANTOS_SFI_DRIVER_READY lease_ms=%u cpus=4\n", lease_ms);
	return 0;
}

static void __exit santos_exit(void)
{
	cancel_delayed_work_sync(&lease_work);
	santos_shutdown(false);
	pr_info("SANTOS_SFI_EXIT enable=%u writes=%u restore_error=%d\n",
		enable, writes, restore_error);
}

module_init(santos_init);
module_exit(santos_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Santos10wifi integration");
MODULE_DESCRIPTION("Observe-only by default, renewable leased Cloverview package CPUFreq driver");
