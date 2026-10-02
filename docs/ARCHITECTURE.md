# Architecture and source map

```text
GNOME Shell 51 ── Mutter Santos backend ── Android HWC ── DRM shell
GTK4 apps ── SantosGL/GLES2 ── Wayland/libhybris ── PowerVR SGX544
Pipeline ── private libmpv ── VDX decoder ── MSVDX kernel engine
PipeWire/WirePlumber ── ALSA UCM ── SST/WM8994 machine driver
                       Linux 7.2 / Void Linux i686
```

The kernel platform code, external SGX provider, VDX modules and userspace are
separate source/build identities. A `uname` string does not identify the other
components. `SOURCE-MANIFEST.json` records source bases and upstream archive hashes;
`SHA256SUMS` freezes the distributed files.

| Path | Purpose |
|---|---|
| `patches/linux/` | Platform, boot, input, storage, USB, audio and power delta against upstream Linux 7.2 |
| `kernel/overlay/` | Current Santos DDK 1.12 provider, including uncommitted fixes; original license headers retained |
| `kernel/configs/` | Accepted HIGHMEM kernel configuration; no PAE; SGX is built externally |
| `modules/santos-sync-core/` | Standalone native fence core matching the accepted HIGHMEM source/header hashes |
| `modules/santos-vdx/` | DRM shell and video engine/MMU source; historical gate sources retained for build completeness |
| `modules/santos-cpufreq/` | Cloverview SFI package CPUFreq driver; explicit enablement, renewable lease and safe control restoration |
| `patches/gtk/` | GTK 4.22.4 SantosGL and application integration changes |
| `patches/mutter/`, `patches/gnome-shell/` | GNOME 51 source deltas |
| `patches/pipeline/` | Current Pipeline 4.1.0 delta, including embedded libmpv and timing fixes |
| `packaging/void/` | Selected recipes/patches; upstream Void tree is required |
| `integration/hybris-wayland/` | Current host Wayland bridge source and build helper |
| `integration/runtime-reference/` | Read-only capture of installed GNOME service and audio configuration |
| `integration/cpufreq/` | Build/boot/instance-pinned managed performance service and supervised rollback helper |
| `integration/firefox/`, `patches/firefox/` | ESR78 compatibility experiment, guarded TLS shim and private EGL fix |
| `configs/` | Recorded Meson user options and runtime-reference hashes |

Old, unrelated PVR DDK variants are omitted. Their historical Kconfig choices
remain in the parent files; use the supplied configuration (`CONFIG_SGX=n`)
and external Santos provider. Selecting another DDK is unsupported.

Vendor HWC/gralloc/EGL/GLES and firmware are separate inputs. No proprietary
binaries or reverse-engineering dumps are bundled. The installed HWC also carries
previous device-specific binary adjustments, including the external-monitor
status correction (L186); this preview does not reproduce those binaries.
