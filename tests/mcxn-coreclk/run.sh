#!/usr/bin/env bash
# MCXN947 the M33 core clock / SysTick is DERIVED from the SCG: time a fixed MRT interval
# (MRT on the 150 MHz bus clock = a real-time reference) with SysTick at the 48 MHz reset
# core clock, then again after raising the core to 150 MHz via PLL0.  The same interval costs
# 150/48 = 3.125x more SysTick ticks -- because SysTick's clock moved.  A model that pinned the
# core to a 150 MHz constant reads the same count both times.
# ⚠ -icount shift=3 REQUIRED: the measurement is in virtual SysTick ticks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/coreclk.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 30 "$QEMU" -M frdm-mcxn947 -icount shift=3 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "CORECLK PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
