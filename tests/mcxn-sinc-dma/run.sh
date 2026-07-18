#!/usr/bin/env bash
# MCXN947 peripheral-triggered eDMA: SINC CIC results moved entirely by the DMA.
#
# The CPU never reads CnRDATA after arming.  Every settled result the SINC computes is
# carried out by the eDMA, one minor loop per SINC0 ch0 FIFO-watermark request (mux source
# 103), when the driver sets CnCCR[DMAEN].  Before that request line was wired, DMAEN drove
# nothing and a DMA-armed SINC waited forever for a request that could not be raised.
#
# The oracle is closed-form and independent of the DMA path: an all-ones bitstream into a
# CIC of order 1, OSR 16 settles to the DC gain 16, so every DMA'd word must read (w>>8)==16.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/sinc-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 30 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "SINC-DMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
