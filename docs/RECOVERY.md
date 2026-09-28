# Recovery and installation boundary

This repository contains source and configuration, not a universal tablet installer.
No included helper writes block devices, flashes partitions, restarts services or
unloads kernel modules.

The development GT-P5210 currently uses normal boot partition p10 for Linux 7.2.
p11 holds the accepted rollback candidate. The original golden p10 image is backed
up off-device. These facts describe one tablet and do not authorize overwriting the
same partition numbers on another device. The `-highmem-p11` kernel release string
is historical; it does not identify the slot currently booted.

Before installation on any device:

1. Establish the exact model, partition map and currently booted image identity.
2. Keep verified off-device backups of original boot/recovery and required data.
3. Demonstrate USB recovery that does not depend on the partition to be written.
4. Validate the assembled boot image and matching modules before choosing a slot.
5. Record the previous/new image hashes and a tested way back.

The publication pass performed read-only SSH inspection and local builds only.
It did not reboot, flash, modify packages or certify recovery on a new device.
Do not unload SGX/VDX while DMA, mappings or userspace references may remain active.
A timed-out DMA wait does not prove that hardware has stopped.

The [boot/storage engineering record](BOOT-AND-STORAGE.md) explains the S-Boot
handoff, dual-copy PARAM recovery fix and alternate-GPT partition consolidation.
The [image-release assessment](IMAGE-RELEASE.md) lists the remaining installer work.
