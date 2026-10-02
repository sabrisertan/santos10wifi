# Cloverview CPU frequency and managed performance

## Why this driver was added

The original 3.4 port consumed Cloverview's SFI frequency table. The current
7.2 build had CPUFreq/SFI core support but no matching frequency driver; the
generic Intel/ACPI drivers did not bind. Real two-core load measured about
800 MHz despite the firmware advertising 800, 933, 1333 and 1600 MHz.

The new external driver uses that known firmware control path, not an invented
voltage or overclock. Controlled request probes showed a shared package clock;
one policy covers all four logical CPUs. CPU0's observed zero request register
is kept read-only while other existing requests control the package.

## Ready-made aggressive policy

The service selects Linux's existing **performance** governor. It requests the
policy maximum immediately rather than choosing a load threshold. The profile
allows 800000–1600000 kHz and requests 1600 MHz even at idle. Battery/brightness
caps or a custom governor were not added; there is a battery/heat cost. Existing
CPU-idle and hardware thermal protections are not disabled. Native deep-idle
support itself remains a separate port gap, not supplied by this governor.

The kernel lease is 60 s, renewed by the manager every 10 s. Manager loss causes
policy unregister and original-control restoration. Build ID, boot, process
identity and an immutable per-load token prevent replacing another instance;
every renewal/release is checked in the kernel. Unknown controls or failed
restore are not force-overwritten/unloaded. This does not establish complete
thermal, hotplug or suspend support.

## Build and prepare — no installation or hardware action

After the [matching kernel build](../REPRODUCING.md):

```sh
tools/build-cpufreq.sh out/kernel out/cpufreq-new
python3 tools/prepare-cpufreq-service.py out/kernel \
  out/cpufreq-new/santos-sfi-cpufreq.ko out/cpufreq-bundle-new
```

The full `tools/build-kernel.sh` also builds `out/kernel/cpufreq/`. Bundle
preparation uses the actual kernel `vmlinux` and module build IDs, verifies
vermagic/config, and hashes service files. It requires Python, readelf and
modinfo. Existing output directories are refused. These helpers do not load,
install, flash, connect to a device or change CPU frequency. Matching source
alone does not mean your kernel ELF matches the running tablet.

## Existing-development-device installation boundary

No universal installer is provided. On an already supported tablet/rootfs:

1. Verify exact hardware, firmware table, **running** kernel build ID and the
   newly built module. Keep the working rootfs/services and independent recovery.
2. Check that no other CPUFreq manager/module owns the device. Copy the generated
   bundle to a **previously absent** `/opt/santos-cpufreq/`; never overwrite an
   unknown existing deployment.
3. Create a **new** `/etc/sv/santos-cpufreq/` using bundle `service-run` as `run`
   and `service-log-run` as `log/run`; both must be executable. Stage it disabled
   and link it from `/var/service/santos-cpufreq` on a runit rootfs.
4. Only after verifying identity, manifest, readiness and rollback, remove that
   task-owned `down` marker and start the new service. Confirm `santos-sfi`, one
   `policy0` covering CPUs 0–3, `performance`, actual frequency and thermal health.
5. Record the exact output/deployed/loaded identities. A source-preview build is
   not automatically the maintainer's runtime. Do not copy an old `.ko`, invent
   hashes or force module loading on a different kernel.

Unsupported kernel build IDs are skipped with no clock action. No kernel/slot
image or GNOME/GPU/VDX service changes are required by the CPUFreq service.
System suspend and independent end-user installation remain unaccepted.

## Stop / rollback

As root on an installed development device:

```sh
python3 -B /opt/santos-cpufreq/disable-service.py
```

The helper checks the task-owned service alias/run, leaves it down across boots
and requests supervised shutdown. The manager releases its own instance,
verifies original controls before unload, and checks policy removal. Inspect
`/run/santos-cpufreq/state.json`; a command exit code alone is not a complete
rollback proof. Do not unload graphics/video modules to recover this service.

## Recorded acceptance, not a general device-limit claim

- Host: 105 control/table and 221 package-transaction checks, sanitizer runs,
  service ownership/validation checks and matching external builds passed.
- Device: four real states, package load, normal release, lease expiry, manager
  crash, duplicate-manager refusal, token rejection, owned recovery and runit
  stop/re-enable were tested. The managed service is active on the development
  tablet; a cold boot into the final service was not part of that task.
- A matched 1080p30/300-frame fixture at 800 MHz had 190 sampled VO drops. At
  1600 MHz it decoded all 300 frames with zero sampled VO/decoder drops and the
  maintainer confirmed smooth video. Player/input/options/graphics/audio were
  matched, but boot/memory epochs differed; this is not a repeated same-boot
  statistical A/B or acceptance of all media.
- Contiguous-buffer allocation, VDX timer completion, hot debug logging and
  native CPU-idle gaps remain separately tracked. Sustained thermal, hotplug,
  suspend, network/corrupt/long playback and full-device acceptance are not
  implied by the frequency result.

See [validation](VALIDATION.md) and [known issues](KNOWN-ISSUES.md).
