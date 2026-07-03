#!/usr/bin/env bash
# MCXN947 UART board-to-board link: LPUART2 over a chardev socket, byte-exact
# echo round-trip with a peer.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
PY="${PY:-python3}"
ELF="$HERE/uartlink.elf"
PORT="${UART_PORT:-14990}"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v "$PY" >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
      -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
CONSOLE="$(mktemp)"
"$QEMU" -M frdm-mcxn947 -display none -monitor none \
    -serial "file:$CONSOLE" \
    -chardev "socket,id=ul,host=127.0.0.1,port=$PORT,server=on,wait=off" -serial chardev:ul \
    -kernel "$ELF" -no-reboot &
QPID=$!
sleep 0.5
"$PY" "$HERE/uart_peer.py" "$PORT"
# poll the console up to ~8s for the result line
for _ in $(seq 1 80); do
    grep -qE "UART LINK (PASS|FAIL)" "$CONSOLE" 2>/dev/null && break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
OUT="$(cat "$CONSOLE" 2>/dev/null)"; rm -f "$CONSOLE"
echo "--- guest console ---"; echo "$OUT"; echo "---------------------"
echo "$OUT" | grep -q "UART LINK PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
