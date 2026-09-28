#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Compile a new candidate. No installation, packaging, flashing, or SSH.
set -eu
[ "$#" = 2 ] || { echo "Usage: $0 PREPARED_KERNEL NEW_BUILD_DIR" >&2; exit 2; }
release=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel=$(CDPATH= cd -- "$1" && pwd)
[ ! -e "$2" ] || { echo "Build directory must not exist" >&2; exit 2; }
mkdir -p "$2"
out=$(CDPATH= cd -- "$2" && pwd)
cp "$release/kernel/configs/santos10wifi-highmem.config" "$out/.config"
make -C "$kernel" O="$out" ARCH=x86 LOCALVERSION=-santos-preview olddefconfig
make -C "$kernel" O="$out" ARCH=x86 LOCALVERSION=-santos-preview -j"${JOBS:-4}" bzImage modules
cp -R "$release/modules/santos-sync-core" "$out/sync-core"
make -C "$kernel" O="$out" ARCH=x86 LOCALVERSION=-santos-preview M="$out/sync-core" \
 -j"${JOBS:-4}" modules
cp -R "$release/modules/santos-vdx" "$out/vdx"
cp -R "$release/kernel/overlay/drivers/gpu/drm/pvrsgx/santos-112" "$out/pvr"
make -C "$kernel" O="$out" ARCH=x86 LOCALVERSION=-santos-preview M="$out/vdx" \
 PVR112="$out/pvr" -j"${JOBS:-4}" modules
# The provider imports the VDX interrupt dispatch; do not import unrelated symbols.
awk '$2 == "santos_pvr_irq_start" || $2 == "santos_pvr_irq_stop" {print}' "$out/vdx/Module.symvers" > "$out/vdx-provider.symvers"
make -C "$kernel" O="$out" ARCH=x86 LOCALVERSION=-santos-preview M="$out/pvr" \
 CONFIG_DRM_PVRSGX_SANTOS_112=m KBUILD_EXTRA_SYMBOLS="$out/vdx-provider.symvers" \
 -j"${JOBS:-4}" modules
printf 'Built candidate in %s\n' "$out"
