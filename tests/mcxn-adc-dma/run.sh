#!/usr/bin/env bash
# ADC conversions captured entirely by the eDMA (ADC0 FIFO A = mux source 21).
# The CPU never reads RESFIFO: if the request line does not work, the destination
# stays poisoned (0xEEEEEEEE) and this fails.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/adc-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
# -icount: a DETERMINISTIC instrument.  Without it this test was a COIN FLIP --
# it passed ~1 run in 3, and every green was luck.  The wall clock decided whether
# the eDMA drained the ADC FIFO between triggers, and a test whose verdict depends
# on host load is not an instrument, it is a mood.  (Under -icount it failed 4/4 --
# which is how the depth-1-FIFO data-loss bug finally got caught.)
OUT="$(timeout 90 "$QEMU" -M frdm-mcxn947 -display none -monitor none -icount shift=3 \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "ADCDMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
