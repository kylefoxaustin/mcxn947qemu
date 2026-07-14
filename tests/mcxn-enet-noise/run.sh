#!/bin/bash
# A CORRUPTION DETECTOR MUST NOT CRY FOUL AT TRAFFIC THAT WAS NEVER ITS PROTOCOL.
#
# holobench ran the real 4-node lab -- Linux peers on the wire -- and found:
#
#     mcx     REJECTS 0x86dd x5      <- IPv6.  The Linux kernels' multicast NDP/MLD.
#
# We were body-checking EVERY non-self frame, so a neighbour-discovery packet was
# validated against a beacon body it was never going to have and reported as
# ENET-LAB3 CORRUPT -- a token their scorer greps as a HARD FAIL.  This node would
# have failed the lab because a peer said hello.
#
#   ⭐ A DETECTOR THAT CRIES FOUL AT TRAFFIC THAT WAS NEVER ITS PROTOCOL WILL BE
#     TURNED OFF BY THE PEOPLE IT PROTECTS.
#
# AND MY OWN SUITES COULD NEVER HAVE FOUND IT: every node on my segment speaks the
# beacon protocol, so foreign traffic cannot arise.  A suite can be exhaustive within
# its own model of the world and still be BLIND BY CONSTRUCTION to everything outside
# it.  So this suite puts the outside world ON the wire: a node armed with BEACON_NOISE
# emits IPv6, and the honest nodes must IGNORE it -- not count it, not condemn it.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE/../.."
QEMU=build/qemu-system-arm; CC=${CC:-arm-none-eabi-gcc}; L3="$HERE/../mcxn-enet-lab3"
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"; kill -KILL -${P1:-0} -${P2:-0} -${P3:-0} 2>/dev/null' EXIT

B() { "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
   -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DMY_MAC_LSB=$4 ${6:-} \
   -T "$L3/link.ld" "$L3/main.c" -o "$5"; }
B 0x88B5 0x88B6 0x88B7 0x01 "$T/mcx.elf"   || { echo "SKIP: build failed"; exit 0; }
B 0x88B6 0x88B5 0x88B7 0x02 "$T/rt.elf"    || { echo "SKIP: build failed"; exit 0; }
B 0x88B7 0x88B5 0x88B6 0x03 "$T/noise.elf" -DBEACON_NOISE || { echo "SKIP: build failed"; exit 0; }
cmp -s "$T/noise.elf" "$T/mcx.elf" && { echo "FAIL: BEACON_NOISE did not land"; exit 1; }

M="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
echo "segment: mcast=$M  (one node emits IPv6, like a Linux peer would)"
rn() { setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
   -nic socket,mcast=$M,model=mcxn-enet,mac=$2 -kernel "$1" -no-reboot >"$3" 2>/dev/null & echo $!; }
P1=$(rn "$T/mcx.elf"   54:27:8d:00:00:01 "$T/1.log")
P2=$(rn "$T/rt.elf"    54:27:8d:00:00:02 "$T/2.log")
P3=$(rn "$T/noise.elf" 54:27:8d:00:00:03 "$T/3.log")
sleep 9
kill -KILL -"$P1" -"$P2" -"$P3" 2>/dev/null

corrupt=$(grep -c 'ENET-LAB3 CORRUPT' "$T/1.log")
lost=$(grep -c 'ENET-LAB3 LOST' "$T/1.log")
rc=0
# ① The IPv6 traffic must be IGNORED -- never reported as corruption.
[ "$corrupt" -eq 0 ] || {
    echo "FAIL: cried CORRUPT at non-beacon traffic ($corrupt times):"
    grep -m2 'ENET-LAB3 CORRUPT' "$T/1.log" | sed 's/^/        /'
    echo "      holobench's scorer greps that token as a HARD FAIL."
    rc=1; }
# ② And the noise must not be mistaken for a peer either: mcx sees only rt1180 (0x88B6),
#    never both, so it must NEVER pass -- silence about the noise, not credit for it.
[ "$(grep -c 'ENET-LAB3 PASS' "$T/1.log")" -eq 0 ] || {
    echo "FAIL: counted an IPv6 emitter as a beacon peer"; rc=1; }
echo "   CORRUPT lines at IPv6 traffic: $corrupt (must be 0)   LOST: $lost"
[ $rc -eq 0 ] && echo "PASS: foreign traffic is ignored -- not counted, not condemned"
exit $rc
