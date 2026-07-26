#!/usr/bin/env bash
# MCXN947 USBFS/KHCI HOST-mode enumeration: the MCX enumerates a real QEMU usb-kbd attached to
# the controller's own usb-bus. The guest KHCI host driver runs SETUP/IN/OUT control transfers
# to read the device's DEVICE descriptor and SET_ADDRESS. PASS = "USB-HOST ENUM OK".
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/usb-host.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(timeout -k 5 20 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -kernel "$ELF" -no-reboot -device usb-kbd,bus=usb-bus.0,port=1 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "USB-HOST ENUM OK" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
