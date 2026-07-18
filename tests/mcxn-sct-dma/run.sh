#!/usr/bin/env bash
# MCXN947 SCT-event-paced eDMA: a const pattern in flash copied into SRAM word-by-word,
# one word per SCT event (CMSIS SCT0 DMA0 = source 19, gated by DMAREQ0[DEV_0]), the CPU
# never touching the destination.  Before the SCT request line was wired, it moved nothing.
#
# Two axes: DATA (the SRAM buffer must end byte-exact equal to the flash pattern) and
# RATE (one word per event -> the 8-word transfer takes >= 7 self-calibrated SCT periods,
# measured against SysTick).  ⚠ -icount shift=3 REQUIRED: the RATE axis times virtual time.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/sct-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 15 "$QEMU" -M frdm-mcxn947 -icount shift=3 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "SCT-DMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
