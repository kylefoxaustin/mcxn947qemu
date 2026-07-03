#!/usr/bin/env bash
# MCXN947 CAN board-to-board link: FlexCAN0 <-> can-bus <-> can-host-chardev
# <-> socket <-> peer; byte-exact frame round-trip BOTH directions.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
PY="${PY:-python3}"
ELF="$HERE/canlink.elf"
PORT="${CAN_PORT:-14970}"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v "$PY" >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
      -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
CONSOLE="$(mktemp)"
# Fleet-standard wiring: -object can-bus,id=cb -machine canbus0=cb (same shape
# as i.MX 91/93/95 + the holobench CAN labs).
"$QEMU" -M frdm-mcxn947 -display none -monitor none -serial "file:$CONSOLE" \
    -object can-bus,id=cb -machine canbus0=cb \
    -chardev "socket,id=canl,host=127.0.0.1,port=$PORT,server=on,wait=off" \
    -object can-host-chardev,id=h0,canbus=cb,chardev=canl \
    -kernel "$ELF" -no-reboot &
QPID=$!
sleep 0.5
PEER="$("$PY" "$HERE/can_peer.py" "$PORT")"
for _ in $(seq 1 40); do
    grep -qE "CAN LINK (PASS|FAIL)" "$CONSOLE" 2>/dev/null && break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
OUT="$(cat "$CONSOLE" 2>/dev/null)"; rm -f "$CONSOLE"
echo "--- guest console ---"; echo "$OUT"; echo "--- peer ---"; echo "$PEER"; echo "------------"
# PASS = peer->MCX verified (console) AND MCX->peer received (peer ok>=1)
PEEROK="$(echo "$PEER" | grep -oE 'ok=[0-9]+' | grep -oE '[0-9]+')"
if echo "$OUT" | grep -q "CAN LINK PASS" && [ "${PEEROK:-0}" -ge 1 ]; then
    echo "PASS"; exit 0
else
    echo "FAIL"; exit 1
fi
