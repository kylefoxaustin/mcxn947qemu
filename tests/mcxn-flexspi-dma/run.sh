#!/usr/bin/env bash
# MCXN947 peripheral-triggered eDMA: a flash read carried entirely by the DMA.
#
# The CPU programs a known pattern into a real m25p80 NOR, arms an eDMA channel at RFDR,
# sets IPRXFCR[RXDMAEN], and NEVER reads RFDR itself.  Every word must come back byte-exact,
# carried by the DMA on the FlexSPI RX request (mux source 1).  Before that request line was
# wired, RXDMAEN drove nothing and a DMA-driven flash read hung waiting for a request that
# could not be raised.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexspi-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 20 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "FLEXSPI-DMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
