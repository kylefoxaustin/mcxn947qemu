#!/usr/bin/env bash
# MCXN947 LPTMR runs on its PSR[PCS]-selected LOW-POWER clock (RM Table 463), not the 150 MHz
# bus clock.  Measures FRO_12M (PCS=00) absolutely vs SysTick (12.5 ticks/count, catches the
# old 150 MHz = 1.0) and the PCS=00-vs-32K per-count ratio (~366, proves the selector works).
# ⚠ -icount shift=3 REQUIRED: the measurement is in virtual SysTick ticks.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/lptmr.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 60 "$QEMU" -M frdm-mcxn947 -icount shift=3 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "LPTMR PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
