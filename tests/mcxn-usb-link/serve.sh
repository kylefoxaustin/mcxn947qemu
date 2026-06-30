#!/usr/bin/env bash
# M4 — bring the MCX up as the usbredir SERVER end of the i.MX93 <-> MCX USB
# link.  Stays in the foreground waiting for the host (i.MX93) to connect.
#
#   ./serve.sh [hs|fs]      hs = USBHS/ChipIdea (default), fs = USBFS/KHCI
#
# The i.MX93 side connects as the CLIENT with stock QEMU (no model coupling):
#   -chardev socket,id=ur0,path=$USB_SOCK,server=off,reconnect-ms=2000 \
#   -device usb-redir,chardev=ur0
# (reconnect-ms, not the deprecated reconnect= seconds form — QEMU 11.x.)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
CTRL="${1:-hs}"
SOCK="${USB_SOCK:-/tmp/holo-usb-imx93-mcx.sock}"
case "$CTRL" in
    hs) ID=mcxn-usbhs; SRC=$HERE/../mcxn-usb-hs; ELF=usbhs.elf;;
    fs) ID=mcxn-usbfs; SRC=$HERE/../mcxn-usb;    ELF=usb.elf;;
    *)  echo "usage: $0 [hs|fs]"; exit 2;;
esac
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$SRC/link.ld" "$SRC/main.c" -o "$SRC/$ELF"
rm -f "$SOCK"
echo "MCX usbredir server ($CTRL) listening at $SOCK — connect the i.MX93 client."
echo "(Ctrl-C to stop; the server stays up persistently, serving every (re)connect.)"
# IMPORTANT: do NOT use '-serial mon:stdio' here.  Run non-interactively (e.g.
# from a lab coordinator) stdin is closed, and mon:stdio quits QEMU on stdin
# EOF — which tore the server down mid-enumeration in the first live pairing.
# Detach from stdin and keep the console off the monitor so the server lives
# until killed, serving the full enumeration and every reconnect.
exec "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial null \
    -chardev "socket,id=$ID,path=$SOCK,server=on,wait=off" \
    -kernel "$SRC/$ELF" -no-reboot </dev/null
