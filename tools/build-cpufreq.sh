#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build one external module against an already prepared matching kernel build.
# No installation, module loading, SSH, or flash.
set -eu
[ "$#" = 2 ] || { echo "Usage: $0 MATCHING_KERNEL_BUILD NEW_MODULE_DIR" >&2; exit 2; }
release=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
kernel_build=$(CDPATH= cd -- "$1" && pwd)
[ -f "$kernel_build/Module.symvers" ] || { echo "Kernel build lacks Module.symvers" >&2; exit 2; }
[ -f "$kernel_build/include/config/kernel.release" ] || { echo "Kernel release file missing" >&2; exit 2; }
[ ! -e "$2" ] || { echo "Module directory must not exist" >&2; exit 2; }
mkdir -p "$2"
out=$(CDPATH= cd -- "$2" && pwd)
cp "$release/modules/santos-cpufreq/Makefile" "$out/Makefile"
cp "$release/modules/santos-cpufreq/santos-sfi-cpufreq.c" "$out/santos-sfi-cpufreq.c"
cp "$release/modules/santos-cpufreq/santos-cpufreq-core.h" "$out/santos-cpufreq-core.h"
make -C "$kernel_build" ARCH=x86 M="$out" -j"${JOBS:-4}" modules
printf 'Built CPUFreq module in %s (not loaded or installed)\n' "$out"
