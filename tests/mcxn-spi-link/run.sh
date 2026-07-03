#!/usr/bin/env bash
# MCXN947 SPI board-to-board link: FlexComm5 LPSPI master <-> spi-link <-> socket
# <-> peer; byte-exact framed-pattern round-trip.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
PY="${PY:-python3}"
ELF="$HERE/spilink.elf"
PORT="${SPI_PORT:-14980}"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v "$PY" >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
      -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
CONSOLE="$(mktemp)"
"$QEMU" -M frdm-mcxn947 -display none -monitor none \
    -serial "file:$CONSOLE" \
    -chardev "socket,id=spil,host=127.0.0.1,port=$PORT,server=on,wait=off" \
    -device spi-link,bus=mcxn-lpspi,chardev=spil \
    -kernel "$ELF" -no-reboot &
QPID=$!
sleep 0.5
"$PY" "$HERE/spi_peer.py" "$PORT"
for _ in $(seq 1 40); do
    grep -qE "SPI LINK (PASS|FAIL)" "$CONSOLE" 2>/dev/null && break
    sleep 0.1
done
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
OUT="$(cat "$CONSOLE" 2>/dev/null)"; rm -f "$CONSOLE"
echo "--- guest console ---"; echo "$OUT"; echo "---------------------"
echo "$OUT" | grep -q "SPI LINK PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
