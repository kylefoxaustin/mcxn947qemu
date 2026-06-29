#!/usr/bin/env bash
# MCXN947 USB device-mode enumeration test: a bare-metal KHCI gadget enumerates
# end-to-end against a self-contained usbredir host over a socket.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
PY="${PY:-python3}"
ELF="$HERE/usb.elf"
PORT="${USB_PORT:-14761}"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v "$PY" >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

SERIAL="$(mktemp)"
"$QEMU" -M frdm-mcxn947 -display none -monitor none \
    -serial "file:$SERIAL" \
    -chardev "socket,id=usb,host=127.0.0.1,port=$PORT,server=on,wait=off" \
    -global mcxn-usbdev.chardev=usb \
    -kernel "$ELF" -no-reboot &
QPID=$!

"$PY" "$HERE/usbredir_host.py" 127.0.0.1 "$PORT"
RC=$?

sleep 0.3
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

OUT="$(cat "$SERIAL" 2>/dev/null)"
rm -f "$SERIAL"
echo "--- guest console ---"; echo "$OUT"; echo "---------------------"
# End-to-end PASS requires BOTH ends: the host verified descriptors (RC=0) AND
# the device firmware reached SET_CONFIGURATION ("USB ENUM OK").
if [ "$RC" -eq 0 ] && echo "$OUT" | grep -q "USB ENUM OK"; then
    echo "PASS"; exit 0
else
    echo "FAIL (host rc=$RC)"; exit 1
fi
