# Reproducing the Santos port

**The published sources and kernel/module build can be reconstructed. The complete
running tablet cannot yet be reproduced from this repository alone.** This guide
separates the verified steps from the inputs and installation work still missing.

## What “reproduced” means here

| Outcome | Current evidence / remaining limit |
|---|---|
| Reconstruct the modified Linux, GTK, Mutter, Shell and Pipeline sources | Pinned upstream inputs plus patches; changed-file comparisons passed |
| Build Linux and external SGX/VDX/fence modules | Host build passed, including the separately packaged `santos-sync-core` |
| Produce byte-identical binaries | Not established; recorded binary hashes identify a particular build, not a deterministic-build guarantee |
| Rebuild the full GNOME/Void userspace | Incomplete: private dependencies, vendor inputs and a clean bootstrap recipe remain |
| Install the photographed system on a fresh tablet | Not yet validated: rootfs, first boot, image assembly, storage migration and independent recovery still need an end-to-end procedure |

The README screenshot shows the maintainer's working development tablet. It does
not establish fresh-device reproducibility. Target hardware is GT-P5210 /
`santos10wifi`; other Tab 3 models are outside the acceptance scope.

## 1. Get and check the source preview

Use a Linux host with Python 3.12+, Git, GNU make, an x86 32-bit-capable compiler,
binutils, flex, bison, bc, OpenSSL development headers and libelf development
headers. Downloads below also use curl. See the recorded compiler and output
identities in [`configs/build-validation.json`](configs/build-validation.json).
A hermetic toolchain/dependency container is not provided.

```sh
git clone https://github.com/sabrisertan/santos10wifi.git
cd santos10wifi
git rev-parse HEAD
python3 tools/verify-release.py
python3 -B -m unittest discover -s tests -v
mkdir -p work
```

Record the Santos repository commit you use. `SHA256SUMS` checks source, patches,
configuration and build/checking tools. Markdown is excluded from checksum
matching so documentation edits in GitHub do not require regenerating it; Markdown
still undergoes private-data and local-link checks. Editing code/config requires a
reviewed manifest update. A complete frozen archive can separately be identified
by its archive hash.

## 2. Prepare and build the kernel and modules

```sh
git clone --depth 1 --branch v7.2 https://github.com/torvalds/linux.git work/linux-upstream
python3 tools/prepare-source.py linux work/linux-upstream work/linux-santos
JOBS=4 tools/build-kernel.sh work/linux-santos out/kernel
```

The helper requires a clean upstream checkout and a **new** destination/output
path. For another attempt, use another output directory rather than overwriting
the previous evidence. The manifest's Linux `base` is the peeled commit
`8d3ae59288f1e7d58d76558a6ee96d533bc5019f`; the annotated `v7.2` tag object is
recorded separately. These IDs are different; `git rev-parse HEAD` returns the
commit ID, not the tag object ID.

Expected outputs include:

- `out/kernel/arch/x86/boot/bzImage` and the selected in-tree modules.
- `out/kernel/sync-core/santos-sync-core.ko` — the standalone native fence core.
- `out/kernel/vdx/santos-pvr-shell-vdx.ko` and `santos-vdx.ko`.
- `out/kernel/pvr/santos-pvr112.ko` and the other selected provider modules.

The build does not load modules, create a root filesystem or flash anything.
`bzImage` still needs the correct Android boot wrapper/ramdisk and matching runtime
assets. The kernel release includes `-santos-preview`. The original config also
contains a historical localversion hash; use the manifest and source inputs to
identify the new build rather than interpreting that embedded hash as its source
commit. Only the supplied config and Santos provider path are supported by this
trimmed DDK source preview.

## 3. Reconstruct the desktop/application source trees

For GTK, clone the pinned 4.22.4 release (the helper verifies its commit):

```sh
git clone --depth 1 --branch 4.22.4 https://github.com/GNOME/gtk.git work/gtk-upstream
python3 tools/prepare-source.py gtk work/gtk-upstream work/gtk-santos
```

For the three archive-based components:

```sh
curl --fail --location --output work/mutter-51.0.tar.xz \
  https://download.gnome.org/sources/mutter/51/mutter-51.0.tar.xz
curl --fail --location --output work/gnome-shell-51.0.tar.xz \
  https://download.gnome.org/sources/gnome-shell/51/gnome-shell-51.0.tar.xz
curl --fail --location --output work/pipeline-4.1.0.tar.gz \
  https://gitlab.com/schmiddi-on-mobile/pipeline/-/archive/4.1.0/pipeline-4.1.0.tar.gz

python3 tools/prepare-source.py mutter work/mutter-51.0.tar.xz work/mutter-santos
python3 tools/prepare-source.py gnome-shell work/gnome-shell-51.0.tar.xz work/shell-santos
python3 tools/prepare-source.py pipeline work/pipeline-4.1.0.tar.gz work/pipeline-santos
```

Each archive must match the SHA256 in
[`SOURCE-MANIFEST.json`](SOURCE-MANIFEST.json). A failed fetch or checksum is a stop
condition; do not silently use the latest release or change the expected digest.
Mutter/Pipeline `base` IDs refer to local development baselines; their public
reconstruction inputs are the pinned archives, not upstream Git commits with those
IDs. The `source_head` fields likewise record development provenance.

These commands produce patched **sources**. They do not build or install the
private desktop. [`docs/BUILDING.md`](docs/BUILDING.md) describes the package overlay,
Meson option records and Wayland bridge inputs. Apply the Pipeline patch once:
the package recipe already carries the same cumulative source delta.

## 4. What is missing for a clean desktop/device reproduction

| Missing integration | Required next deliverable |
|---|---|
| Clean Void i686 base and dependency closure | Pin package artifacts/versions, build environment and installation order; complete a fresh rootfs assembly |
| Private GNOME bundle | Reconstruct the GJS/ICU/introspection and other private dependencies around the published Mutter/Shell sources |
| GTK and client routing | Fetch matching upstream subprojects and provision the recorded runtime/client helpers; prove clean i686 builds |
| PowerVR/HWC/gralloc/firmware | Inventory exact vendor inputs and their provenance/distribution terms; provide an owner-supplied extraction path where needed |
| Private Wayland/EGL ABI | Build the matching libhybris bridge and private `libwayland-egl`; the distro library is not ABI-equivalent |
| Current HWC binary adjustments | Reproduce and verify the recorded device-specific changes; the installed proprietary blob is not shipped here |
| Boot and first-boot environment | Supply a reviewed ramdisk/wrapper, module selection, service layout and unique account/SSH/network setup |
| Storage and recovery | Implement a guarded migration for the exact device and independently test installation/rollback |

Some reference launchers use `/root/santos`, `/opt`, desktop user `santos` and UID
1001. Those paths document the development layout; copying the scripts alone
cannot provision their dependencies. The Firefox experiment also has the security
and compatibility limits in [`SECURITY.md`](SECURITY.md).

For the actual device changes and remaining image work, read
[`BOOT-AND-STORAGE.md`](docs/BOOT-AND-STORAGE.md),
[`RECOVERY.md`](docs/RECOVERY.md) and
[`IMAGE-RELEASE.md`](docs/IMAGE-RELEASE.md).

## 5. Record the result at the level actually tested

Keep the Santos commit, upstream commit/archive hashes, config, compiler version,
commands, exit codes and output hashes. Different timestamps, toolchains or build
environments can change output hashes; matching a source baseline is not proof of
a byte-reproducible binary build.

For a later device test, record the boot identity and **loaded** module build IDs,
then test graphics, sound, input, networking, playback and recovery independently.
The existing [validation record](docs/VALIDATION.md) describes what has passed so
far. GitHub Actions checks the source package and offline regressions; it does not
boot a tablet or rebuild the complete GNOME system.
