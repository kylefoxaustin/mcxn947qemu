#!/usr/bin/env bash
# Two MCX instances on a point-to-point QEMU socket link (listen/connect):
# prove a frame transmitted by one node is received by the other over the wire.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/enet-x2.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
# TWO nodes, DISTINGUISHABLE ON THE WIRE.  One ELF for both meant neither could tell its
# own echo from the peer's -- and the ISR was not looking at the frame anyway.
E1="${ELF%.elf}-1.elf"; E2="${ELF%.elf}-2.elf"
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -DNODE_ID=1 -T "$HERE/link.ld" "$HERE/main.c" -o "$E1"
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -DNODE_ID=2 -T "$HERE/link.ld" "$HERE/main.c" -o "$E2"
PORT=$(( (RANDOM % 20000) + 20000 ))
O1="$(mktemp)"; O2="$(mktemp)"
qrun() { timeout -k 5 9 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -nic "socket,$1,model=mcxn-enet,mac=52:54:00:00:00:0$2" \
        -kernel "$3" -no-reboot; }
qrun "listen=127.0.0.1:$PORT" 1 "$E1" >"$O1" 2>/dev/null & P1=$!
sleep 1                                          # let node1's listener bind
qrun "connect=127.0.0.1:$PORT" 2 "$E2" >"$O2" 2>/dev/null & P2=$!
wait $P1 $P2 || true
echo "--- node1 (listen) ---"; cat "$O1"; echo "--- node2 (connect) ---"; cat "$O2"; echo "-------------"
n1=$(grep -c "ENETX2 PASS" "$O1" || true); n2=$(grep -c "ENETX2 PASS" "$O2" || true)
c1=$(grep -c "ENETX2 CORRUPT" "$O1" || true); c2=$(grep -c "ENETX2 CORRUPT" "$O2" || true)
rm -f "$O1" "$O2"
[ "$c1" -eq 0 ] && [ "$c2" -eq 0 ] || { echo "FAIL: a peer frame arrived with a WRONG BODY (corrupt=$c1/$c2)"; exit 1; }
[ "$n1" -ge 1 ] && [ "$n2" -ge 1 ] && { echo "PASS (each node VERIFIED the peer frame: ethertype, sender, and 49 payload bytes)"; exit 0; } || { echo "FAIL"; exit 1; }
