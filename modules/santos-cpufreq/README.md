# Cloverview package CPUFreq

External GPL-2.0-only driver for the verified GT-P5210 firmware FREQ/CPUS
tables. Hardware control follows the Intel/Samsung 3.4 SFI CPUFreq source;
modern callbacks, safe MSR access and ownership/restore handling are used.

One policy represents the measured shared package clock across CPUs 0–3.
CPU0's observed zero request remains read-only, but CPU0 joins the policy.
Only advertised 800/933/1333/1600 MHz states are accepted; no voltage or
frequency is invented, and EIST/thermal-control registers are not written.

Default `enable=0` only reads metadata/MSRs. Active mode requires explicit
enablement and a bounded lease. The [managed service](../../docs/CPUFREQ.md)
selects the built-in performance governor and renews its own lease. Lease
expiry unregisters policy and restores tracked controls. Uncertain ownership
is never force-overwritten. A per-load token binds renewal/release to the
manager instance.

Build and deployment preparation are documented in
[REPRODUCING.md](../../REPRODUCING.md). No module binaries are shipped here.
The `tests/cpufreq/` host cases cover table/control/domain failures and rollback;
they do not establish hardware, thermal, hotplug or suspend acceptance.
