#!/bin/bash
# The MRT had NO TEST -- which is why a `?: 150000000` fallback that HAPPENED to equal
# the real clock rate was invisible.  This measures the MRT's interval against SysTick
# (an ABSOLUTE check against an independent clock), because a RATIO test cancels the very
# rate that was being invented.  -icount, because it is a timing measurement.
set -e
cd "$(dirname "$0")/../.."
CC=${CC:-arm-none-eabi-gcc}
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
  -T tests/mcxn-mrt/link.ld tests/mcxn-mrt/main.c -o "$T/mrt.elf" || { echo "SKIP: build failed"; exit 0; }
OUT="$(timeout 120 build/qemu-system-arm -M frdm-mcxn947 -icount shift=3 -display none \
        -monitor none -serial stdio -kernel "$T/mrt.elf" -no-reboot 2>/dev/null || true)"
echo "$OUT" | grep -E 'MRT '
echo "$OUT" | grep -q "MRT PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
