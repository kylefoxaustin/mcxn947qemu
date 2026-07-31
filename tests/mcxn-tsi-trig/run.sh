#!/usr/bin/env bash
# LPTMR0 -> INPUTMUX (TSI_TRIG) -> TSI0 scan.  Software mode ignores the trigger; hardware
# mode (GENCS[STM]) paces a scan per LPTMR compare.  Guest-side; see main.c.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/tsi-trig.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 120 "$QEMU" -M frdm-mcxn947 -display none -monitor none -icount shift=3 \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "TSI-TRIG PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
