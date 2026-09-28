# Direct-install image: feasibility and remaining work

A direct-install release is feasible, but it is a separate engineering task rather
than compressing the current tablet's disk and uploading it. This source preview
does **not** provide a tested installation image.

## What already exists

- An accepted Linux 7.2 boot image, matching external modules, kernel configuration
  and an off-device golden/rollback baseline for the development tablet.
- A working Void i686 GNOME 51 installation, selected package recipes and private
  runtime components. Kernel/module clean-build verification is complete.
- Historical small-partition/GPT backups and a private file-level rootfs backup.
- A documented p8 restore basis, but no completed destructive restore test.

The private rootfs backup includes credentials in their normal filesystem paths.
It also predates the final GNOME/Pipeline/Firefox state. It cannot be published as
an installation payload. A current live-root dump would likewise include personal
state and would not by itself be a consistent, tested installation image.

## Required work before an image release

| Work item | Why it is needed |
|---|---|
| Clean rootfs assembly | Build from a fresh staging root with pinned packages/private runtime, not a personal filesystem copy; preserve ownership, modes, ACLs/xattrs and service dependencies |
| First-boot setup | Generate unique SSH host keys/machine identity, create accounts and network setup; ship no passwords, Wi-Fi profiles, authorized keys or browser data from the maintainer |
| Vendor dependency plan | Inventory required Samsung/Intel/PowerVR firmware and libraries; establish what may be distributed or provide an owner-supplied extraction workflow with hashes |
| Storage migration tooling | Recognize the exact device/layout, preserve device-unique partitions, handle the alternate-GPT policy and refuse unknown layouts; record backup and readback checks |
| Recovery environment | Prove boot and USB access independently of the p8 filesystem being replaced; distinguish known-device rollback from a fresh-user recovery path |
| Boot image assembly | Supply a reviewed ramdisk and correct Android wrapper with matching module load selection, not just bzImage |
| Installation/recovery acceptance | Exercise installation and rollback on an appropriate test device, then cold boots, network/audio/display checks, physical video/browser checks and a 30-minute regression run |

The development p11 rollback reaches the existing system; that fact alone does not
prove a p8-independent installer/recovery environment for another tablet. Historical
Samsung download-mode/Heimdall acceptance and filesystem-restore gaps must be
resolved for the chosen release procedure; normal USB Ethernet on a working rootfs
is a different test.

The remaining work is substantial enough that an image is intentionally deferred
for this publication. The complete source preview and boot/storage documentation
are available now. No raw device dump, private backup or untested image is attached
as a user-installable release asset.
