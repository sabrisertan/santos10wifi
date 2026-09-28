# Validation of the 2026-09-28 source preview

| Check | Result and scope |
|---|---|
| Linux patch round trip | PASS: 99 changed/new files reconstructed against pinned Linux 7.2 and compared byte for byte |
| GTK patch round trip | PASS: 73 changed/new files against GTK 4.22.4, including new renderer/shader sources |
| Mutter patch round trip | PASS: 41 files; public upstream 51.0 tarball reconstruction also matches current changed files |
| GNOME Shell patch | PASS: 8 files against the preserved upstream 51.0 archive; hash-checked public source preparation passed |
| Pipeline patch round trip | PASS: 27 files; public upstream 4.1.0 tarball reconstruction matches current source |
| mpv / libhybris recipe patches | PASS: all eight patches in each recipe apply in order to the preserved upstream archives; no package build claim |
| Kernel and external modules | PASS: final build helper completed in a new output directory, including bzImage, configured kernel modules, VDX and SGX provider modules |
| Source preservation | Original source HEADs and working-tree status sets unchanged |
| Source-package check | SHA256 inventory, internal links/symlinks, Python/shell syntax, binary exclusions and bounded private-data patterns checked by `tools/verify-release.py` |
| Live device | Read-only kernel/loaded-module identity, installed file hashes, process/memory/disk inspection; no installation or reboot |

`configs/build-validation.json` records the **new, unbooted** build hashes and
compiler. `configs/installed-baseline.json` records the **existing tablet** identity.
These are different builds. Compilation does not validate hardware operation.

The first local helper attempt failed at external module modpost because a fresh
kernel needed the `modules` target to emit `Module.symvers`. A second dependency
check identified the two exact shell IRQ exports needed by the provider. Both
helper defects were fixed; the complete final helper then passed from a new output
directory. Earlier failed logs are retained privately, not counted as passes.
The provider still emits its existing missing `MODULE_DESCRIPTION()` warning.

No complete desktop rebuild, fresh rootfs installation, new 30-minute runtime
stress test, cold-boot network/power replay or physical video/browser acceptance
was performed. Previous subsystem acceptance is reported with its original scope
in the known-issues document. GitHub Actions runs the verifier and GPT fixture tests on pushes and pull
requests; consult the [Actions page](https://github.com/sabrisertan/santos10wifi/actions)
for the exact commit result. The private-data scan is bounded and is not a comprehensive
security audit.

## Publication follow-up: boot/storage documentation

The September 28 publication follow-up added the LLM-development attribution,
boot/partition engineering record and installable-image assessment. The GPT
inspector passed five offline tests: mixed Samsung/standard headers, differing
valid arrays, corrupt header/array CRC reporting, excessive array bounds and
out-of-disk array rejection (the first test covers both header variants).
It also ran read-only on the i686 tablet: both GPT headers and arrays validate,
with 21 old primary entries and 18 alternate entries. Disk/partition GUIDs are
omitted from the public reference. An initial ioctl request used the host's
64-bit request size and was corrected to the target's `sizeof(size_t)` before
successful i686 inspection; no write was attempted.

The earlier 299-file validation count belongs to the initial source preview;
the current `SHA256SUMS` and verifier output cover the added files as well.

## README editing and reproduction follow-up

A README edit revealed that the original all-file checksum policy unnecessarily
failed normal documentation updates. Markdown now skips checksum matching while
retaining syntax-independent hygiene/private-data and local-link checks. Code,
patches, configuration and tools remain hash-checked. The test suite covers both
accepted documentation edits and rejected source changes/private data/binaries.

Reproduction review also corrected the Linux manifest from the annotated `v7.2`
tag object to its peeled commit, and included the missing standalone
`santos-sync-core` source/header and build step. Public Linux/GTK tag references
were checked. Linux source preparation passed from a clean checkout of the pinned
commit using the local upstream object mirror; all 99 patched files and the
provider overlay matched the source used for the build. The complete build helper
passed in a new output directory, including the fence core. Updated output hashes
are in `configs/build-validation.json`; these binaries have not been booted.

The eleven offline tests pass locally, including annotated-tag checkout handling
and documentation/source-policy regressions. See [REPRODUCING.md](../REPRODUCING.md)
for the remaining full-desktop and device-installation gaps. Earlier file counts
in this record describe their individual snapshots; current inventory comes from
the source manifest/verifier, which intentionally excludes Markdown hashes.
