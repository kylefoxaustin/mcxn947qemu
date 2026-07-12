#!/usr/bin/env bash
# MCXN947 I3C controller transfer-complete + NVIC interrupt test.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/i3c.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout 10 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -device at24c-eeprom,bus=i2c-bus.0,address=0x50,rom-size=256 -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "I3C PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
