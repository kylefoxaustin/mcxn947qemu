#!/usr/bin/env bash
# INPUTMUX DMAn_REQ_ENABLE really gates the eDMA.  See main.c for why an UNGATED
# model is worse than a missing feature: it is MORE PERMISSIVE THAN THE SILICON, so
# the developer's bug passes here and dies on the board.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/inputmux-gate.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
# -icount: a deterministic instrument.  A gate test whose verdict depends on host
# load is not an instrument, it is a mood.
OUT="$(timeout 90 "$QEMU" -M frdm-mcxn947 -display none -monitor none -icount shift=3 \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "GATE PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
