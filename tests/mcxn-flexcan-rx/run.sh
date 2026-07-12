#!/usr/bin/env bash
# MCXN947 FlexCAN receive path: ID matching + honest overrun + disabled module.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexcan-rx.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
# BOTH FlexCANs on ONE can-bus: CAN0 transmits, CAN1 receives, so the frame
# travels the real board-to-board path (not the separate loopback code path —
# mutation testing showed a loopback test cannot catch bus-path bugs).
OUT="$(timeout 20 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -object can-bus,id=cb -machine canbus0=cb,canbus1=cb \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "CANRX PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
