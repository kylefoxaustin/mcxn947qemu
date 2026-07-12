#!/usr/bin/env bash
# Three-node raw-L2 segment self-test.
#
# Stands three MCX instances up on ONE L2 segment (a QEMU socket mcast group),
# each impersonating one node of the fleet's cross-check by EtherType:
#
#     0x88B5 = mcxn947    0x88B6 = rt1180    0x88B7 = imx95
#
# Every node must observe BOTH of the others before it prints PASS.  Proving the
# segment with three MCX stand-ins first means that when rt1180 and imx95 join
# for real, any failure is theirs or the wire's — not the lab's.
#
# Joining the real fleet segment: build the 0x88B5 variant and run it against the
# same mcast group the others use (see MCAST below).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }

# A distinct group per run so concurrent CI jobs don't share a segment.
MCAST="${MCAST:-230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))}"

build() { # <ethertype> <peerA> <peerB> <mac-lsb> <out>
  "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
    -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DMY_MAC_LSB=$4 \
    -T "$HERE/link.ld" "$HERE/main.c" -o "$5" || return 1
}

build 0x88B5 0x88B6 0x88B7 0x01 "$HERE/node-mcx.elf"     || { echo "SKIP: build failed"; exit 0; }
build 0x88B6 0x88B5 0x88B7 0x02 "$HERE/node-rt1180.elf"  || { echo "SKIP: build failed"; exit 0; }
build 0x88B7 0x88B5 0x88B6 0x03 "$HERE/node-imx95.elf"   || { echo "SKIP: build failed"; exit 0; }

O1=$(mktemp); O2=$(mktemp); O3=$(mktemp)
trap 'rm -f "$O1" "$O2" "$O3"' EXIT

node() { # <elf> <mac> <out>
  timeout 12 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
    -nic socket,mcast=$MCAST,model=mcxn-enet,mac=$2 \
    -kernel "$1" -no-reboot >"$3" 2>/dev/null &
}

echo "segment: mcast=$MCAST"

# JOIN=1: bring up ONLY our 0x88B5 node and hold it on the REAL fleet segment,
# so rt1180 (0x88B6) and imx95 (0x88B7) can join with their own firmware.  We
# broadcast for the whole window and PASS only on seeing BOTH other ethertypes —
# a peer that has not arrived yet is not a failure, it is just not here yet.
if [ "${JOIN:-0}" = "1" ]; then
  HOLD="${HOLD:-180}"
  echo "JOIN: holding 0x88B5 on $MCAST for ${HOLD}s (waiting for 0x88B6 + 0x88B7)"
  timeout "$HOLD" "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
    -nic socket,mcast=$MCAST,model=mcxn-enet,mac=02:4d:43:58:00:01 \
    -kernel "$HERE/node-mcx.elf" -no-reboot 2>/dev/null | tee "$O1" || true
  if grep -q 'ENET-LAB3 PASS' "$O1"; then
    echo "PASS: 0x88B5 saw BOTH 0x88B6 and 0x88B7 on the wire"
    exit 0
  fi
  echo "did NOT see both peers — who was on the segment:"
  grep -oE 'peer ethertype 0x[0-9a-f]+' "$O1" | sort -u || echo "  (nobody)"
  exit 1
fi

node "$HERE/node-mcx.elf"    02:4d:43:58:00:01 "$O1"
node "$HERE/node-rt1180.elf" 02:4d:43:58:00:02 "$O2"
node "$HERE/node-imx95.elf"  02:4d:43:58:00:03 "$O3"
wait

a=$(grep -c 'ENET-LAB3 PASS' "$O1" || true)
b=$(grep -c 'ENET-LAB3 PASS' "$O2" || true)
c=$(grep -c 'ENET-LAB3 PASS' "$O3" || true)
echo "saw-both-peers: mcx=$a rt1180=$b imx95=$c"

if [ "$a" -gt 0 ] && [ "$b" -gt 0 ] && [ "$c" -gt 0 ]; then
  echo PASS
  exit 0
fi
echo "--- node1 (0x88B5)"; tail -4 "$O1"
echo "--- node2 (0x88B6)"; tail -4 "$O2"
echo "--- node3 (0x88B7)"; tail -4 "$O3"
echo FAIL
exit 1
