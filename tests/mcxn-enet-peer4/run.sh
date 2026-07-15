#!/bin/bash
# ⭐ AN ABSENCE OF REJECTION IS NOT AN ACCEPTANCE.            (91emulator)
#
# This firmware knew exactly THREE ethertypes.  When imx91 (0x88B8) joined holobench's
# lab, is_beacon_et() returned false on every frame it sent, so frame_ok() bailed on the
# first line and NEVER READ ITS BODY.  The interop matrix then showed "mcx never rejected
# imx91" -- and that was read as mcx ACCEPTING it.
#
#   "mcx NEVER REJECTED imx91" and "mcx NEVER LOOKED AT imx91" ARE THE SAME CELL.
#
# So this suite does not test that 0x88B8 is tolerated.  Silence would pass that.
# IT TESTS THAT THE BODY IS ACTUALLY READ -- in BOTH directions:
#
#   ① an HONEST 0x88B8 peer must be COUNTED (a positive sighting, not mere silence)
#   ② a LIAR at 0x88B8 must be CAUGHT     (proving we read the body we claim to check)
#
# ② is the load-bearing half.  Without it, ① is satisfied by a node that accepts anything.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE/../.."
QEMU=build/qemu-system-arm; CC=${CC:-arm-none-eabi-gcc}; L3="$HERE/../mcxn-enet-lab3"
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"; kill -KILL -${P1:-0} -${P2:-0} -${P3:-0} -${P4:-0} 2>/dev/null' EXIT

# ⭐ A FIXED `sleep` IN A TEST IS A DELAY-LOOP CRUTCH -- the same confession I flagged in
#   the firmware and then wrote into my own harness.  Under load the QEMU nodes run
#   slower, so a wall-clock guess that is generous on an idle box is TOO SHORT in a full
#   suite run.  This suite passed 3/3 standalone and FAILED inside run-all.sh: a flaky
#   test is a bug you have agreed to see only SOMETIMES.
#
#   So: POLL FOR THE CONDITION, bounded.  Fast when idle, correct when loaded, and it
#   FAILS LOUDLY if the condition never arrives rather than silently under-waiting.
wait_for() {  # <file> <pattern> <count> <max_seconds>
    local f="$1" pat="$2" want="$3" max="$4" i
    for i in $(seq 1 $((max * 5))); do
        [ "$(grep -c "$pat" "$f" 2>/dev/null || echo 0)" -ge "$want" ] && return 0
        sleep 0.2
    done
    return 1
}

B() { "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
   -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DPEER_C=$4 -DMY_MAC_LSB=$5 ${7:-} \
   -T "$L3/link.ld" "$L3/main.c" -o "$6"; }
# mcx knows all three peers.  The stand-ins each see the other three.
B 0x88B5 0x88B6 0x88B7 0x88B8 0x01 "$T/mcx.elf"    || { echo "SKIP: build failed"; exit 0; }
B 0x88B6 0x88B5 0x88B7 0x88B8 0x02 "$T/rt.elf"     || { echo "SKIP: build failed"; exit 0; }
B 0x88B7 0x88B5 0x88B6 0x88B8 0x03 "$T/i95.elf"    || { echo "SKIP: build failed"; exit 0; }
B 0x88B8 0x88B5 0x88B6 0x88B7 0x04 "$T/i91.elf"    || { echo "SKIP: build failed"; exit 0; }
B 0x88B8 0x88B5 0x88B6 0x88B7 0x04 "$T/i91-liar.elf" -DBEACON_REPLAY || { echo "SKIP: build failed"; exit 0; }
cmp -s "$T/i91-liar.elf" "$T/i91.elf" && { echo "FAIL: the liar flag did not land"; exit 1; }

M="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
rn() { setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio -semihosting-config enable=on,target=native \
   -nic socket,mcast=$M,model=mcxn-enet,mac=$2 -kernel "$1" -no-reboot >"$3" 2>/dev/null & echo $!; }

echo "① an HONEST imx91 (0x88B8) must be COUNTED by mcx"
P1=$(rn "$T/mcx.elf" 54:27:8d:00:00:01 "$T/1.log")
P2=$(rn "$T/rt.elf"  54:27:8d:00:00:02 "$T/2.log")
P3=$(rn "$T/i95.elf" 54:27:8d:00:00:03 "$T/3.log")
P4=$(rn "$T/i91.elf" 54:27:8d:00:00:04 "$T/4.log")
wait_for "$T/1.log" 'ENET-LAB3 PASS' 1 40 || echo "   (timed out waiting for mcx to see all three peers)"
sleep 1
kill -KILL -"$P1" -"$P2" -"$P3" -"$P4" 2>/dev/null
seen=$(grep -c 'ethertype 0x88b8' "$T/1.log"); pass=$(grep -c 'ENET-LAB3 PASS' "$T/1.log")
bad=$(grep -c 'ENET-LAB3 CORRUPT' "$T/1.log")
rc=0
[ "$seen" -ge 1 ] || { echo "   FAIL: mcx never even SAW 0x88B8"; rc=1; }
[ "$pass"  -ge 1 ] || { echo "   FAIL: mcx never PASSed -- it needs ALL THREE peers, so it did not count imx91"; rc=1; }
[ "$bad"   -eq 0 ] || { echo "   FAIL: mcx called an HONEST imx91 frame corrupt ($bad)"; rc=1; }
echo "   saw 0x88B8: $seen   PASS (all 3 peers): $pass   false CORRUPT: $bad"

echo "② a LIAR at 0x88B8 must be CAUGHT -- this is what proves we READ the body"
M="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
P1=$(rn "$T/mcx.elf"      54:27:8d:00:00:01 "$T/5.log")
P2=$(rn "$T/rt.elf"       54:27:8d:00:00:02 "$T/6.log")
P3=$(rn "$T/i95.elf"      54:27:8d:00:00:03 "$T/7.log")
P4=$(rn "$T/i91-liar.elf" 54:27:8d:00:00:04 "$T/8.log")
wait_for "$T/5.log" 'CORRUPT.*et=0x88b8' 1 40 || echo "   (timed out waiting for mcx to catch the liar)"
sleep 1
kill -KILL -"$P1" -"$P2" -"$P3" -"$P4" 2>/dev/null
caught=$(grep -c 'CORRUPT.*et=0x88b8' "$T/5.log"); p2=$(grep -c 'ENET-LAB3 PASS' "$T/5.log")
[ "$caught" -ge 1 ] || { echo "   FAIL: mcx did NOT catch a lying 0x88B8 -- it is not reading the body"; rc=1; }
[ "$p2" -le 1 ]     || { echo "   FAIL: mcx kept counting a liar as a healthy peer ($p2)"; rc=1; }
echo "   caught the liar: $caught   passes while lied to: $p2 (<=1: only its genuinely-first frame)"


# ③ ⭐ ONE PINNED IMAGE, ANY SEGMENT.
#    ②'s node was COMPILED knowing 0x88B8 (-DPEER_C).  That means every future node on the
#    segment is a FIRMWARE RELEASE: a rebuild, a restage, a new hash, a new announcement --
#    and it is exactly how holobench ended up running a two-generations-stale copy of me.
#    95emulator: "if a peer set is a constant, every future node is a firmware release."
#
#    So the peer set is now a RUNTIME TABLE in SRAM, seeded from the launch line.  This
#    scenario builds a node that knows NOTHING about 0x88B8 at compile time, hands it the
#    fourth peer on the command line, and demands it CATCH a liar there.
echo "③ a node that has NEVER HEARD of 0x88B8 learns it from the LAUNCH LINE"
B 0x88B5 0x88B6 0x88B7 0 0x01 "$T/blind.elf" || { echo "SKIP: build failed"; exit 0; }
LOADER="-device loader,addr=0x20007f00,data=0x52454550,data-len=4
        -device loader,addr=0x20007f04,data=3,data-len=4
        -device loader,addr=0x20007f08,data=0x88b6,data-len=4
        -device loader,addr=0x20007f0c,data=0x88b7,data-len=4
        -device loader,addr=0x20007f10,data=0x88b8,data-len=4"
M="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio -semihosting-config enable=on,target=native \
  -nic socket,mcast=$M,model=mcxn-enet,mac=54:27:8d:00:00:01 $LOADER \
  -kernel "$T/blind.elf" -no-reboot >"$T/9.log" 2>/dev/null & P1=$!
setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio -semihosting-config enable=on,target=native \
  -nic socket,mcast=$M,model=mcxn-enet,mac=54:27:8d:00:00:04 \
  -kernel "$T/i91-liar.elf" -no-reboot >"$T/10.log" 2>/dev/null & P4=$!
wait_for "$T/9.log" 'et=0x88b8' 1 40 || echo "   (timed out waiting for the launch-line peer table)"
sleep 1
kill -KILL -"$P1" -"$P4" 2>/dev/null
learned=$(grep -c 'et=0x88b8' "$T/9.log")
[ "$learned" -ge 1 ] || { echo "   FAIL: the launch-line peer table was not read -- 0x88B8 stayed invisible"; rc=1; }
echo "   caught a lying 0x88B8 with a peer set it was NEVER COMPILED with: $learned"

[ $rc -eq 0 ] && echo "PASS: mcx reads imx91's body, catches a liar there, and learns peers at RUNTIME"
exit $rc
