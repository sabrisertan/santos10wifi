# Linux on the Samsung Galaxy Tab 3 10.1 Wi-Fi

An experimental Linux 7.2 port for **GT-P5210 / santos10wifi**, with a Void Linux
i686 userspace, GNOME 51, PowerVR SGX544 graphics and MSVDX video decoding.

> **Vibe-coded / LLM-built port.** All project-specific porting and integration
> work was carried out through LLM coding sessions, including OpenAI Codex.
> Maintainer [sabrisertan](https://github.com/sabrisertan) reports writing no code
> by hand: he directed the work, operated the tablet and evaluated the results.
> This describes the Santos port's development process; Linux, GNOME, Void and
> the other upstream projects retain their original authorship and licenses.

This is a **source preview**, for developers with this exact device. It preserves
the working port and its integration changes. A fresh-device installer and a
redistributable root filesystem are not included. Other Tab 3 models are untested.

<img width="1280" height="800" alt="Screenshot From 2026-09-28 11-17-23" src="https://github.com/user-attachments/assets/570a2ca8-cf9d-41b9-a049-98bc398b3072" />

## What works

| Component | Recorded state |
|---|---|
| Kernel | Linux 7.2, bounded HIGHMEM4G, normal boot on the development tablet |
| Display and touch | 1280×800, private Mutter/GNOME 51 HWC backend; touchscreen input |
| Graphics | Legacy PowerVR SGX544 GLES2 through libhybris; custom GTK4 SantosGL renderer |
| Audio | SST/WM8994, PipeWire + WirePlumber, speakers/headphones and volume controls accepted |
| Network | NetworkManager Wi-Fi; USB gadget Ethernet/SSH accepted in earlier device tests |
| Video | MSVDX H.264 decoder, private mpv/libmpv and a Pipeline-derived application |
| Power controls | Backlight and charging integration; system suspend is outside the accepted scope |

The September 28 read-only check confirmed the running kernel, loaded module
build IDs, GNOME/audio processes and installed application hashes. It did not
repeat every hardware test. See [validation](docs/VALIDATION.md) for that boundary.

## Start here

- [Reproducing the port: verified steps and missing inputs](REPRODUCING.md)
- [Build and source reconstruction](docs/BUILDING.md)
- [Known bugs and limitations](docs/KNOWN-ISSUES.md)
- [Architecture and source map](docs/ARCHITECTURE.md)
- [Bootloader compatibility and partition migration](docs/BOOT-AND-STORAGE.md)
- [Device recovery and installation boundary](docs/RECOVERY.md)
- [Installable-image feasibility and remaining work](docs/IMAGE-RELEASE.md)
- [Security assumptions](SECURITY.md)
- [Licenses and upstream credits](LICENSE.md)

The browser experiment uses Firefox ESR 78.15 with its content sandbox disabled.
It is a compatibility demonstration, unsuitable for sensitive browsing. The
video candidate still drops frames under load and awaits final physical acceptance.

## Repository contents

`patches/` contains pinned upstream deltas for Linux, GTK, Mutter, GNOME Shell and
Pipeline. `kernel/overlay/` contains the active SGX provider source;
`modules/` contains the VDX engine and DRM shell. `packaging/void/` carries selected
Void recipes. `integration/` preserves the Wayland bridge, session/audio references,
power policy and Firefox compatibility sources.

Proprietary Android graphics libraries, firmware, boot images, rootfs images,
user profiles, Wi-Fi credentials, SSH keys and private experiment logs are excluded.
The full desktop still depends on device-specific vendor libraries supplied
separately by the device owner.

```sh
python3 tools/verify-release.py
```

This verifies the source package. It does not exercise hardware or certify a
fresh installation. See [contributing](CONTRIBUTING.md) before changing the port.
