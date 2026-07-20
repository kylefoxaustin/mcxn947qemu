#!/usr/bin/env bash
# MCXN947 PLL0 is DERIVED: a CTIMER clocked from PLL0 (SYSCON CTIMERCLKSEL=1) follows the
# APLL registers.  Program PLL0=150MHz, time a CTIMER interval vs SysTick, halve the PLL
# multiplier (150 -> 75 MHz) and re-time: the interval doubles.  A model that reported a
# constant (or 0) for the PLL source could not move between the two measurements.
# ⚠ -icount shift=3 REQUIRED: the measurement is in virtual SysTick ticks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/pll-ctimer.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 30 "$QEMU" -M frdm-mcxn947 -icount shift=3 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "PLL-CTIMER PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
