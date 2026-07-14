#!/usr/bin/env bash
# MCXN947 peripheral-triggered eDMA: SAI audio moved entirely by the DMA.
#
# The CPU never writes TDR.  Every word reaching the transmitter is carried by
# the eDMA, one minor loop per SAI FIFO request (mux source 100), and comes back
# through the board-level TXD->RXD loopback byte-exact.  Before the request lines
# were wired, ERQ was a dead bit and this could not move a single word.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/sai-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 30 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -global mcxn-sai.loopback=on -kernel "$ELF" -no-reboot \
        2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "SAIDMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
