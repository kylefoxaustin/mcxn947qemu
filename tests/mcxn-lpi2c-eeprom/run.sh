#!/usr/bin/env bash
# FlexComm0 LPI2C master -> a REAL at24c EEPROM attached to its I2C bus.  The guest writes
# two bytes and reads them back; the EEPROM (not the model) is the oracle.  See main.c.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/lpi2c-eeprom.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 30 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot \
        -device at24c-eeprom,bus=flexcomm0-i2c,address=0x50,rom-size=256 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "LPI2C-EEPROM PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
