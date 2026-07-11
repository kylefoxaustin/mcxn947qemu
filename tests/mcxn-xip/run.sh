#!/usr/bin/env bash
# XIP: code executes in place from the FlexSPI AHB window.
#
# Two assertions, and the second is the one that matters:
#   1. the routine runs from the QSPI window and returns the right answer;
#   2. it does so INSIDE A TIME BUDGET.
#
# QEMU can fetch instructions from an MMIO region — it just won't cache the
# translation block (accel/tcg/translator.c), so an XIP window that regressed
# from memory_region_init_rom_device() to memory_region_init_io() still WORKS,
# and still prints PASS, while running ~100x slower.  A purely functional test
# cannot see that; only a wall clock can.  Hence XIP_BUDGET.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"; CC="${CC:-arm-none-eabi-gcc}"; ELF="$HERE/xip.elf"
XIP_BUDGET="${XIP_BUDGET:-5}"    # measured: rom_device <1s, an MMIO window ~18s
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

start=$(date +%s)
OUT="$(timeout "$XIP_BUDGET" "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -kernel "$ELF" -no-reboot 2>/dev/null | grep -m1 -E 'XIP (PASS|FAIL)' || true)"
elapsed=$(( $(date +%s) - start ))

if [ -z "$OUT" ]; then
  echo "XIP did not complete within ${XIP_BUDGET}s."
  echo "The window is probably not a ROM device: an init_io XIP window still executes,"
  echo "but TCG cannot cache its translation blocks, so it runs ~100x slower."
  echo FAIL; exit 1
fi
echo "$OUT  (${elapsed}s of a ${XIP_BUDGET}s budget)"
[ "${OUT#XIP PASS}" != "$OUT" ] && { echo PASS; exit 0; } || { echo FAIL; exit 1; }
