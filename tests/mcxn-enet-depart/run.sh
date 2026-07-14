#!/bin/bash
# A DEPARTURE MUST BE A POSITIVE ASSERTION, NOT A SILENCE.
#
# Three nodes on one L2 segment.  Kill one.  The survivors must NAME the loss
# (ENET-LAB3 LOST) -- exactly once, at the edge -- and must NOT name the peer that
# is still alive.  Both halves matter:
#
#   a detector that never fires is decoration;
#   a detector that fires on a LIVE peer MANUFACTURES a departure that never
#   happened, and a lab scoring on that reports a wire failure that did not occur.
#   A FALSE 'LOST' IS WORSE THAN NO 'LOST'.
#
# This suite exists because five earlier attempts at it "failed" for five different
# reasons, none of them the firmware:  a log that outran the node (it was watching its
# own backlog);  a spin-count timeout with NO good value (too long = never fires, too
# short = flaps);  a STALE BINARY from a build that failed;  and finally a kill that
# hit the `timeout` wrapper instead of QEMU, so the peer NEVER DIED.
#   ⭐ A KILL THAT REACHES THE WRAPPER AND NOT THE PROCESS IS NOT A KILL, and
#     "the peer departed" and "I killed a shell" were the same observation.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE/../.."
QEMU=build/qemu-system-arm
CC=${CC:-arm-none-eabi-gcc}
L3="$HERE/../mcxn-enet-lab3"
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }

T=$(mktemp -d); trap 'rm -rf "$T"; kill -KILL -$P1 -$P3 2>/dev/null' EXIT
build() {
  "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
    -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DMY_MAC_LSB=$4 \
    -T "$L3/link.ld" "$L3/main.c" -o "$5"
}
# A stale ELF is how you test a build that failed.  Build into a FRESH dir, and abort.
build 0x88B5 0x88B6 0x88B7 0x01 "$T/mcx.elf" || { echo "SKIP: build failed"; exit 0; }
build 0x88B6 0x88B5 0x88B7 0x02 "$T/rt.elf"  || { echo "SKIP: build failed"; exit 0; }
build 0x88B7 0x88B5 0x88B6 0x03 "$T/i95.elf" || { echo "SKIP: build failed"; exit 0; }

M="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
echo "segment: mcast=$M"
# setsid: each node is its own process GROUP, so `kill -PGID` can actually REACH it.
rn() { setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
   -nic socket,mcast=$M,model=mcxn-enet,mac=$2 -kernel "$1" -no-reboot >"$3" 2>/dev/null & echo $!; }
P1=$(rn "$T/mcx.elf" 54:27:8d:00:00:01 "$T/1.log")
P2=$(rn "$T/rt.elf"  54:27:8d:00:00:02 "$T/2.log")
P3=$(rn "$T/i95.elf" 54:27:8d:00:00:03 "$T/3.log")
sleep 6

pre1=$(grep -c LOST "$T/1.log"); pre3=$(grep -c LOST "$T/3.log")
kill -KILL -"$P2" 2>/dev/null || kill -KILL "$P2" 2>/dev/null
sleep 1
if kill -0 "$P2" 2>/dev/null; then
    echo "INCONCLUSIVE: the peer did not die -- the kill did not reach it."
    exit 1     # a kill that does not kill is not a departure, and not a result
fi
sleep 5

a=$(grep -c '0x88b6 went quiet' "$T/1.log")   # the peer we KILLED
b=$(grep -c '0x88b7 went quiet' "$T/1.log")   # the peer still ALIVE
c=$(grep -c '0x88b6 went quiet' "$T/3.log")
rc=0
[ "$pre1" -eq 0 ] && [ "$pre3" -eq 0 ] || { echo "FAIL: LOST fired while every peer was alive (flapping)"; rc=1; }
[ "$a" -eq 1 ] || { echo "FAIL: mcx did not name the departed peer exactly once (got $a)"; rc=1; }
[ "$b" -eq 0 ] || { echo "FAIL: mcx declared a LIVE peer lost ($b times) -- a manufactured departure"; rc=1; }
[ "$c" -eq 1 ] || { echo "FAIL: imx95 did not name the departed peer exactly once (got $c)"; rc=1; }
kill -KILL -"$P1" -"$P3" 2>/dev/null
[ $rc -eq 0 ] && echo "PASS: a departure is NAMED, once, and a live peer is never accused"
exit $rc
