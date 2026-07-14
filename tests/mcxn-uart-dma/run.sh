#!/usr/bin/env bash
# LPUART TX carried entirely by the eDMA (FlexComm4 Tx = request-mux source 78).
# The CPU never writes LPUART_DATA — if the request line does not work, NOTHING
# is printed and this fails on absent output.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/uart-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 20 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output (written ONLY by the DMA) ---"; echo "$OUT"; echo "---------------------------------------------"
echo "$OUT" | grep -q "UARTDMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
