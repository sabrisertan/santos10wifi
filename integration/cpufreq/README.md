# Managed performance integration

These project-specific CPUFreq service and rollback helpers are GPL-2.0-only.
They accompany the [Cloverview external driver](../../modules/santos-cpufreq/README.md).

`santos-cpufreq-service.py` owns one build/boot/instance-pinned module and selects
the existing Linux **performance** governor. It is not a custom governor. The
normal profile is 800000–1600000 kHz with a 60 s lease renewed every 10 s.
Performance requests the maximum even at idle; battery/brightness caps are not
added and native thermal controls are retained.

The service verifies its generated manifest before loading. An expired lease
cannot be revived. The kernel restores controls if the manager is lost; an
owned dead-manager instance can subsequently be recovered. Other instances or
unverified restoration are refused. The rollback helper disables only this
service and requires verified restoration before unloading its CPUFreq module.

Use the [build/preparation instructions](../../docs/CPUFREQ.md). Generate the
manifest from **your matching kernel/module build**; copying the development
tablet's hashes or an arbitrary `.ko` is not an installation procedure. This
source preview does not automatically provision runit or a fresh rootfs.
