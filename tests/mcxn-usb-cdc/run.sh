#!/usr/bin/env bash
# MCXN947 CDC-ACM gadget: enumerate as CDC (2 interfaces) + bulk data echo over
# usbredir, with the core's gadget-profile=cdc so the layout binds cdc_acm.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
PY="${PY:-python3}"
ELF="$HERE/cdc.elf"
PORT="${USB_PORT:-14791}"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v "$PY" >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

SERIAL="$(mktemp)"
"$QEMU" -M frdm-mcxn947 -display none -monitor none -serial "file:$SERIAL" \
    -chardev "socket,id=mcxn-usbhs,host=127.0.0.1,port=$PORT,server=on,wait=off" \
    -global mcxn-usbdev.gadget-profile=cdc \
    -kernel "$ELF" -no-reboot &
QPID=$!
"$PY" "$HERE/cdc_host.py" 127.0.0.1 "$PORT"
RC=$?
sleep 0.3; kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
OUT="$(cat "$SERIAL" 2>/dev/null)"; rm -f "$SERIAL"
echo "--- guest console ---"; echo "$OUT"; echo "---------------------"
if [ "$RC" -eq 0 ] && echo "$OUT" | grep -q "CDC ENUM OK"; then
    echo "PASS"; exit 0
else
    echo "FAIL (host rc=$RC)"; exit 1
fi
