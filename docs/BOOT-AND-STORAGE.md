# Bootloader compatibility and the Android-to-Void storage migration

This port includes the boot and storage work that made Linux and GNOME possible,
not just the desktop changes. The existing Samsung S-Boot loader is retained.
The Linux handoff adaptations are in the published kernel patch; the PARAM repair
and filesystem migration are documented below. No replacement S-Boot binary is
included.

## Linux entry and decompression

The device's legacy boot path does not provide every input expected by the modern
x86 decompressor. The Linux delta includes:

| Area | Implementation in the reconstructed kernel |
|---|---|
| Legacy setup boundary | `arch/x86/boot/Makefile` and `setup.ld`: Santos uses 512-byte setup alignment instead of the upstream 4096-byte boundary; EFI stub is excluded for this configuration |
| Decompressor entry/relocation | `arch/x86/boot/compressed/head_32.S`, `Makefile`, `vmlinux.lds.S`, `misc.c` and related files: board-specific legacy handoff, relocation/decompression support and early tracing |
| Missing loader `init_size` | In the Santos non-relocatable path, `head_32.S` repairs a zero `boot_params.hdr.init_size` from the linked exact value before relocation arithmetic; a nonzero loader value is preserved |
| Header and kernel entry | `arch/x86/boot/header.S`, `arch/x86/kernel/head_32.S`, `head32.c`, `setup.c`, `traps.c` and `init/main.c`: the board's early-entry/debug and platform integration changes |

These changes are already contained in
[`patches/linux/0001-santos.patch`](../patches/linux/0001-santos.patch), not missing
external patches. The snapshot preserves the accepted development source,
including diagnostic code; it is not represented as an upstream-ready patch series.

## Boot image wrapper

The working boot image is an Android-format wrapper around the x86 kernel and an
initramfs. The accepted HIGHMEM image was assembled using the previously verified
p11 image as a metadata/ramdisk anchor, replacing the kernel payload and preserving
header fields. The recorded wrapper uses:

- Kernel load address `0x10008000`, ramdisk address `0x11000000`, 2048-byte pages.
- Historical repacking also preserved `second_addr` (`0x10f00000`) even with an
  empty second payload; the installed `mkbootimg` otherwise zeroed it.
- Full slot size: 20 MiB; the assembled candidate was padded and read back in full.
- Same ramdisk hash before/after the HIGHMEM kernel replacement:
  `9a70e54e8796aa0edba3df5aa6d09c3e6f482c6e8587067123e0291a16215586`.

This documents the accepted assembly, not a sufficient fresh-device installation
recipe. The ramdisk, module selection, rootfs contents and wrapper must agree.
A newly compiled `bzImage` alone is not the flashable image.

## Persistent recovery loop: PARAM has two relevant copies (L155)

An Android/recovery session had left `boot_mode = 4` (`REBOOT_MODE_RECOVERY`) in
PARAM. Linux did not clear that Samsung-specific state, so ordinary power-on kept
selecting recovery despite the intended normal-boot choice.

Investigation found the relevant words at byte offsets `0x400` and `0x300200`
inside the development tablet's 4 MiB PARAM partition, p16. The latter belonged to
a shadow copy starting at `0x300000`. After a backup, both words were cleared;
physical power-on then returned to the intended normal boot. Subsequent HIGHMEM
promotion used normal p10 boot with the rollback p11 preserved.

Those are **observed offsets for the investigated layout**, not a generic command
to write another tablet's PARAM. This repository supplies no automatic PARAM writer.
A boot image filename or a `-p11` kernel suffix cannot prove the current boot slot.

## Partition consolidation and direct Void root

The September 2026 migration replaced the Android data/system allocation with a
single large ext4 Void root at p8. The GPT slot number stayed p8; entries p6
(CACHE), p9 (USERDATA), and p13 (HIDDEN) were removed from the new table. Boot,
recovery, EFS, PARAM and other retained small partitions kept their recorded
positions. This was a filesystem migration, not simply renaming an Android
partition.

The read-only September 28 inspection reconfirmed this layout on the development
16 GB eMMC (30,769,152 sectors, 512 bytes per sector):

| GPT entry | Old primary layout: start / sectors | Active alternate layout: start / sectors |
|---|---|---|
| p8 | SYSTEM: 286720 / 4833280 | VOIDROOT: 286720 / 30470144 (about 14.53 GiB) |
| p13 | HIDDEN: 5120000 / 204800 | Removed |
| p6 | CACHE: 5324800 / 716800 | Removed |
| p9 | USERDATA: 6041600 / 24715264 | Removed |
| p10 | BOOT: 106496 / 40960 | Preserved |
| p11 | RECOVERY: 147456 / 40960 | Preserved |
| p16 | PARAM: 73728 / 8192 | Preserved |

The historical migration preserved raw small-partition backups, GPT head/tail
backups and a project/userspace archive before reformatting. The prepared Void
i686 tree was restored with numeric ownership into an ext4 filesystem compatible
with the then-used 3.4 kernel (no 64bit/metadata-checksum feature requirement).
The initramfs mounted p8 and `switch_root` entered runit. A staged telnet-hold
bring-up preceded the later normal automatic boot; those were distinct gates.
Linux 7.2 subsequently replaced the kernel while retaining the direct-p8 Void root.

### Why the two GPTs deliberately differ

The current read-only check found:

- Primary GPT: 512-byte Samsung header, 21 used entries, valid header and array CRCs.
- Alternate GPT: 92-byte header, 18 used entries, valid header and array CRCs.
- Different arrays: the primary still describes the old Android layout.

Historical tests reported that writes to primary LBA 1–33 appeared successful in
cached reads but did not persist under direct reads/reboot. That was attributed
to eMMC write protection in the investigation. This publication's read-only check
confirms the table divergence; it does not independently prove that hardware cause.

The kernel therefore prefers a valid alternate GPT when both tables are valid
but their array CRCs differ **and the target disk qualifies for the Santos quirk**.
L05 scoped this policy through `block_device_operations.prefer_alternate_gpt`,
`mmc_blk_prefer_alternate_gpt()` in `drivers/mmc/core/block.c`, and selection in
`block/partitions/efi.c`. It is limited to the board's non-removable, block-addressed
MMC path; ordinary disks retain the upstream primary-first selection behavior.
The related Rust block-operations initializer is also carried in the patch.

Do not “repair” this disk by copying the stale primary over the alternate table.
That would restore the old layout view over the enlarged root filesystem. A generic
partition tool reporting a disagreement does not establish which table should win.

The identifier-free reference is
[`configs/partition-layout-reference.json`](../configs/partition-layout-reference.json).
To inspect your own full-disk image without changing it:

```sh
python3 tools/inspect-gpt.py /path/to/full-disk-backup.img
```

The helper opens its input read-only, validates GPT CRCs and prints partition
geometry without disk/partition GUIDs. It can also read a block device when given
appropriate read permission. It never repairs tables or recommends a write.

## Publication evidence boundary

Boot wrapper, PARAM repair and migration descriptions derive from the project's
preserved first-boot, alternate-GPT, L05, L155 and HIGHMEM deployment records.
Current GPT geometry/CRC agreement was re-read for this publication. No repartition,
PARAM write, boot flash, filesystem restore or reboot was performed in this pass.
See [recovery](RECOVERY.md) and [image release](IMAGE-RELEASE.md) before planning an
installation on another device.
