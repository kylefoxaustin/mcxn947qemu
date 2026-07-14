#!/bin/bash
# A STALE FRAME IS A VALID FRAME.  THE QUESTION WAS NEVER "IS THIS FRAME VALID"
# -- IT WAS ALWAYS "IS THIS FRAME *NEW*".                            (91emulator)
#
# The beacon body carries magic, a self-consistent ethertype, and a known fill pattern.
# A buffer whose RX writeback NEVER LANDED still holds the previous good frame from the
# same peer -- so it passes ALL THREE.  That is rt1180's NETC bug (frames DMA'd to guest
# physical address ZERO, 88 of them in the join-late case), and the checker built to
# catch it COULD NOT SEE IT.  I tested it against a CORRUPTED frame and never a STALE one.
#
# The sequence number was in the frame from the first commit -- and never read.
#
#   ⭐ ASSERT ON A NUMBER GOING UP.
#       seq <= last   -> a REPLAYED/STALE buffer.  CORRUPT.  Not a peer sighting.
#       seq >  last+1 -> LOSS.  A logged statistic, NEVER a failure.
#
# This suite arms one node to LIE (a frozen sequence -- every frame perfectly well-formed
# and stale) and demands the honest nodes catch it.  The liar is the SAME PROGRAM behind
# one flag, so the test cannot pass by testing something else -- and the flag is asserted
# to have LANDED before any result is believed:
#   ⭐ A BROKEN BUILD PRODUCES A QUIET, PLAUSIBLE, WRONG NUMBER, AND
#     "MY NEW ASSERTION FOUND NOTHING" IS A VERY COMFORTABLE THING TO BELIEVE.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE/../.."
QEMU=build/qemu-system-arm; CC=${CC:-arm-none-eabi-gcc}; L3="$HERE/../mcxn-enet-lab3"
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"; kill -KILL -${P1:-0} -${P2:-0} -${P3:-0} 2>/dev/null' EXIT

# ⭐ POLL FOR THE CONDITION, DO NOT SLEEP A GUESS.  A fixed `sleep` is generous on an idle
#   box and TOO SHORT inside a full suite run, where a dozen QEMUs compete -- which is
#   exactly how mcxn-enet-peer4 passed 3/3 standalone and FAILED in run-all.sh.
#   A flaky test is a bug you have agreed to see only SOMETIMES.
wait_for() {  # <file> <pattern> <count> <max_seconds>
    local f="$1" pat="$2" want="$3" max="$4" i
    for i in $(seq 1 $((max * 5))); do
        [ "$(grep -c "$pat" "$f" 2>/dev/null || echo 0)" -ge "$want" ] && return 0
        sleep 0.2
    done
    return 1
}

B() { "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
   -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DMY_MAC_LSB=$4 ${6:-} \
   -T "$L3/link.ld" "$L3/main.c" -o "$5"; }
B 0x88B5 0x88B6 0x88B7 0x01 "$T/mcx.elf"  || { echo "SKIP: build failed"; exit 0; }
B 0x88B7 0x88B5 0x88B6 0x03 "$T/i95.elf"  || { echo "SKIP: build failed"; exit 0; }
B 0x88B6 0x88B5 0x88B7 0x02 "$T/liar.elf" -DBEACON_REPLAY || { echo "SKIP: build failed"; exit 0; }
B 0x88B6 0x88B5 0x88B7 0x02 "$T/long.elf" -DBEACON_LONG || { echo "SKIP: build failed"; exit 0; }
cmp -s "$T/long.elf" "$T/mcx.elf" && { echo "FAIL: BEACON_LONG did not land"; exit 1; }
cmp -s "$T/liar.elf" "$T/mcx.elf" && { echo "FAIL: the liar is byte-identical to the honest node -- BEACON_REPLAY did not land"; exit 1; }

M="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
echo "segment: mcast=$M  (node 0x88B6 is ARMED TO REPLAY)"
rn() { setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
   -nic socket,mcast=$M,model=mcxn-enet,mac=$2 -kernel "$1" -no-reboot >"$3" 2>/dev/null & echo $!; }
P1=$(rn "$T/mcx.elf"  54:27:8d:00:00:01 "$T/1.log")
P2=$(rn "$T/liar.elf" 54:27:8d:00:00:02 "$T/2.log")
P3=$(rn "$T/i95.elf"  54:27:8d:00:00:03 "$T/3.log")
wait_for "$T/1.log" 'PAYLOAD-REPLAY' 20 40 || echo "   (timed out waiting for replays)"
sleep 1
kill -KILL -"$P1" -"$P2" -"$P3" 2>/dev/null

wf=$(grep -cE 'BAD-MAGIC|SELF-ET-MISMATCH|BAD-PATTERN' "$T/1.log")
rp=$(grep -c 'PAYLOAD-REPLAY' "$T/1.log")
ps=$(grep -c 'ENET-LAB3 PASS' "$T/1.log")
lost=$(grep -c '0x88b6 went quiet' "$T/1.log")
live=$(grep -c '0x88b7 went quiet' "$T/1.log")
rc=0
# ① The stale frames are WELL-FORMED.  This is the whole point: the old checker's three
#    tests all pass on them, so a nonzero count here means the liar is malformed and the
#    experiment is not testing what it claims.
[ "$wf" -eq 0 ] || { echo "FAIL: liar's frames were malformed ($wf) -- this does not test STALENESS"; rc=1; }
# ② Only freshness can see them.
[ "$rp" -gt 10 ] || { echo "FAIL: replays not caught (got $rp) -- a stale frame is passing as fresh"; rc=1; }
# ③ The liar's FIRST frame is legitimately new; every one after it is not.
[ "$ps" -le 1 ] || { echo "FAIL: the liar was counted as a live peer $ps times"; rc=1; }
# ④ A peer that says nothing NEW has, in every sense that matters, gone QUIET.
[ "$lost" -eq 1 ] || { echo "FAIL: the replaying peer was not declared lost (got $lost)"; rc=1; }
[ "$live" -eq 0 ] || { echo "FAIL: the HONEST peer was accused ($live) -- a manufactured departure"; rc=1; }
echo "   well-formed stale frames: $wf   replays caught: $rp   passes: $ps   liar declared lost: $lost"

# ── ② A FLAWLESS BEACON IN AN OVERSIZED FRAME.  Every content clause passes; only the
#    LENGTH clause can see it.  This is rt1180's 1000-byte frame.
M2="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
rn2() { setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
   -nic socket,mcast=$M2,model=mcxn-enet,mac=$2 -kernel "$1" -no-reboot >"$3" 2>/dev/null & echo $!; }
Q1=$(rn2 "$T/mcx.elf"  54:27:8d:00:00:01 "$T/4.log")
Q2=$(rn2 "$T/long.elf" 54:27:8d:00:00:02 "$T/5.log")
Q3=$(rn2 "$T/i95.elf"  54:27:8d:00:00:03 "$T/6.log")
wait_for "$T/4.log" 'BAD-LENGTH' 20 40 || echo "   (timed out waiting for oversized frames)"
sleep 1
kill -KILL -"$Q1" -"$Q2" -"$Q3" 2>/dev/null
lenbad=$(grep -c 'BAD-LENGTH' "$T/4.log")
content=$(grep -cE 'BAD-MAGIC|SELF-ET-MISMATCH|BAD-PATTERN|PAYLOAD-REPLAY' "$T/4.log")
[ "$lenbad" -gt 10 ] || { echo "FAIL: an oversized frame with a valid beacon prefix was NOT caught (got $lenbad)"; rc=1; }
[ "$content" -eq 0 ] || { echo "FAIL: the long frame failed a CONTENT clause ($content) -- then this does not test LENGTH"; rc=1; }
echo "   oversized frames caught by LENGTH alone: $lenbad   (content failures: $content -- must be 0)"

[ $rc -eq 0 ] && echo "PASS: a stale frame is a VALID frame; and a flawless beacon in a long frame is not a beacon"
exit $rc
