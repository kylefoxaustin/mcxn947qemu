#!/usr/bin/env bash
# MCXN947 USBFS/KHCI HOST mass storage (MSC/BOT): the MCX enumerates an attached usb-storage
# device and reads block 0 via SCSI READ(10) over Bulk-Only Transport, verifying the ACTUAL
# backing-image contents (a host file the model cannot fabricate). PASS = "USB-MSC READ OK".
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/usb-host-msc.elf"
IMG="$HERE/usbdisk.img"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

# Build a 64 KiB raw disk whose block 0 carries a known signature the firmware verifies.
python3 - "$IMG" <<'PY'
import sys
p=sys.argv[1]; d=bytearray(64*1024)
d[0:8]=b"MCXN-USB"; d[508:512]=b"\xDE\xAD\xBE\xEF"
open(p,"wb").write(d)
PY

OUT="$(timeout -k 5 25 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -kernel "$ELF" -no-reboot \
        -drive if=none,id=d0,file="$IMG",format=raw \
        -device usb-storage,bus=usb-bus.0,port=1,drive=d0 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "USB-MSC READ OK" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
