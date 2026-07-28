#!/usr/bin/env bash
# Per-destination selector divergence: ADC0_TRIG=8 -> CTIMER3 M3, ADC1_TRIG=8 -> CTIMER3
# M2 (same written value, different physical match).  CTIMER3 fires only M2, so ADC1
# converts and ADC0 does not.  See main.c.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/adc-ctimer-adc1-trig.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 120 "$QEMU" -M frdm-mcxn947 -display none -monitor none -icount shift=3 \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "ADC1-TRIG PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
