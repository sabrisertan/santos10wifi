# Building and reconstructing sources

Start with [REPRODUCING.md](../REPRODUCING.md) for the verified scope, exact
source preparation sequence and the remaining whole-system gaps.

Use Linux, Python 3.12+ (tar extraction filters), Git, GNU make, a compiler with
x86 32-bit kernel support, binutils, flex, bison, bc, OpenSSL development files
and libelf development files. Userspace requires a separate Void i686 build
environment and its dependencies. No tool here installs to the tablet.

## Kernel and external modules

From this repository's root, choose directories outside the upstream checkout:

```sh
python3 tools/verify-release.py
git clone --depth 1 --branch v7.2 https://github.com/torvalds/linux.git work/linux-upstream
python3 tools/prepare-source.py linux work/linux-upstream work/linux-santos
JOBS=4 tools/build-kernel.sh work/linux-santos out/kernel
```

The helper rejects a dirty or wrong upstream HEAD and an existing destination.
The build creates `arch/x86/boot/bzImage`, `sync-core/santos-sync-core.ko`, `vdx/*.ko` and `pvr/*.ko` under the output
directory. Its release suffix is `-santos-preview`, deliberately identifying a
new candidate. The four running baseline modules are `santos_pvr112`,
`santos_sync_core`, `santos_pvr_shell_vdx`, and `santos_vdx`; other compiled
experimental modules are not installation recommendations.

`bzImage` is **not** a Samsung flashable boot image. Ramdisk assembly, boot header,
module load sequencing, firmware, root filesystem and recovery are separate.
Read [RECOVERY.md](RECOVERY.md) before any device work.

## GTK, Mutter, Shell and Pipeline source

For GTK, use a clean checkout of commit
`7f99ab1a26408b6499a18f353f081e3c0598ea5c` from
`https://github.com/GNOME/gtk.git`, then:

```sh
python3 tools/prepare-source.py gtk /path/to/gtk-4.22.4 work/gtk-santos
```

For Mutter 51.0, GNOME Shell 51.0 and Pipeline 4.1.0, download the `upstream` URL
in `SOURCE-MANIFEST.json`. The helper checks the archive SHA256 before extraction:

```sh
python3 tools/prepare-source.py mutter /path/to/mutter-51.0.tar.xz work/mutter-santos
python3 tools/prepare-source.py gnome-shell /path/to/gnome-shell-51.0.tar.xz work/shell-santos
python3 tools/prepare-source.py pipeline /path/to/pipeline-4.1.0.tar.gz work/pipeline-santos
```

Local development base commit IDs for Mutter/Pipeline are provenance, not commits
to fetch from upstream. The pinned archives are the public reconstruction inputs.
Recorded Meson user options are under `configs/`; dependency versions, private
GJS/ICU/GNOME bundles and cross-environment setup still need a clean bootstrap
recipe. GTK also requires its upstream sassc/libsass subprojects. Full desktop
rebuild/reinstallation was not validated in this publication pass.

## Void package overlay

The recipes originate from Void packages commit
`03a2d96ec0f89c34e5407ed161d1baec2a5f21fd`. Copy the selected recipe directories
into a separate checkout of `https://github.com/void-linux/void-packages.git`.
Inspect conflicts with existing packages before building in an i686 masterdir.
`mpv-santos-devel` is a sibling symlink to the `mpv-santos` template.

`pipeline-santos` carries a single cumulative patch against its hash-pinned
4.1.0 tarball. This replaces an older patch stack which applied successfully but
missed the current `player.rs`, `video_page.rs` and `external_player.rs` contents.
Do not apply both this recipe patch and `patches/pipeline/0001-santos.patch`.

The mpv recipe includes the opt-in VO decoupling fix; Pipeline explicitly sets
`SANTOS_VO_LIBMPV_DECOUPLE=1`. Native mpv additionally requires
`SANTOS_WAYLAND_LEGACY_EGL=1` on this EGL 1.4 stack.

The libhybris recipe alone is not the deployed Wayland overlay. Its matching
Halium base is pinned in the recipe; the current `wayland_window.cpp` and pool
header are under `integration/hybris-wayland/`. That helper expects matching
libhybris `hybris/{include,egl,common}` at its `src/` path and installed Android
7.1/hybris headers. Its private `libwayland-egl` ABI must match the platform
module. Do not substitute the system Wayland EGL library.

Session and audio files in `integration/runtime-reference/` are reference inputs,
not an installer. The GNOME launcher depends on the private bundle and explicit
UID/path conventions. The Firefox launcher is likewise an exact lab reference;
its TLS shim path and private runtime must be provisioned before it can run.
