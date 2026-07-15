#!/bin/bash
# A REBOOT IS NOT A REPLAY -- AND THE SEQUENCE NUMBER ALONE CANNOT TELL THEM APART.
#                                                    (rt1180emulator / 95emulator)
#
# A peer that RESTARTS resets its sequence to 1.  On seq alone that is seq<=last -- exactly
# a replay -- so the freshness check that catches a stale buffer ALSO condemns an honest
# reboot.  A bigger-jump rule cannot save it: a stalled RX ring hands back seq 1 too.  The
# information to tell them apart was NEVER ON THE WIRE; the fleet added it -- a per-boot
# INCARNATION at [24..27], drawn from the ELS DTRNG (which reseeds from host entropy every
# power-on, so it genuinely differs boot to boot).
#
# This suite proves the distinction with a control that DISABLES it:
#
#   A  honest peer (per-boot nonce) is KILLED and RESTARTED  -> observer prints REBOOT,
#      NOT replay.  The reset sequence is accepted as a fresh baseline.
#   C  the SAME procedure with -DINCARN_CONSTANT (a constant wearing a nonce's name)
#      -> observer condemns the restart as PAYLOAD-REPLAY and prints NO reboot.  The bug
#      RETURNS the instant the nonce stops being per-boot.  (95emulator's case C.)
#   D  a -DBEACON_LEGACY peer (no incarnation, sentinel 0x5A5A5A5A) -> counted as SEEN,
#      never CORRUPT, no reboot/replay verdict: freshness UNVERIFIABLE, not falsely red.
#
#   ⭐ A TEST THAT CANNOT REPRODUCE THE BUG WITH THE FIX DISABLED HAS NOT PROVEN THE FIX
#     IS LOAD-BEARING.  Case C is that reproduction, and the observer/peer are the SAME
#     PROGRAM behind a flag, asserted to have LANDED (cmp), so the control cannot pass by
#     testing something else.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE/../.."
QEMU="${QEMU:-build/qemu-system-arm}"; CC="${CC:-arm-none-eabi-gcc}"; L3="$HERE/../mcxn-enet-lab3"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }
T=$(mktemp -d)
# ⚠ `kill -KILL -0` targets the CALLER'S OWN process group -- suicide.  A PGID of 0 (an
#   unset/cleared handle) must NEVER reach kill.  Guard every process-group kill.
kpg() { local g; for g in "$@"; do case "$g" in ''|0) ;; *) kill -KILL -"$g" 2>/dev/null;; esac; done; }
OBS=0; PEER=0
trap 'rm -rf "$T"; kpg "$OBS" "$PEER"' EXIT

# ⭐ POLL FOR THE CONDITION, DO NOT SLEEP A GUESS.  grep -c exits 1 on zero matches, so
#   `grep -c || echo 0` would emit "0\n0" -- default an empty capture to 0 instead.
wait_for() {  # <file> <pattern> <count> <max_seconds>
    local f="$1" pat="$2" want="$3" max="$4" i n
    for i in $(seq 1 $((max * 5))); do
        n=$(grep -c "$pat" "$f" 2>/dev/null); n=${n:-0}
        [ "$n" -ge "$want" ] && return 0
        sleep 0.2
    done
    return 1
}

B() { "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
   -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DMY_MAC_LSB=$4 ${6:-} \
   -T "$L3/link.ld" "$L3/main.c" -o "$5"; }

# The observer (0x88B5) watches EXACTLY ONE peer, 0x88B6, via the runtime peer table.
B 0x88B5 0x88B6 0x88B7 0x01 "$T/obs.elf"                     || { echo "SKIP: build failed"; exit 0; }
# The peer (0x88B6) in three flavours -- all the SAME program behind a flag.
B 0x88B6 0x88B5 0x88B7 0x02 "$T/peer.elf"                    || { echo "SKIP: build failed"; exit 0; }
B 0x88B6 0x88B5 0x88B7 0x02 "$T/peer_const.elf" -DINCARN_CONSTANT || { echo "SKIP: build failed"; exit 0; }
B 0x88B6 0x88B5 0x88B7 0x02 "$T/peer_legacy.elf" -DBEACON_LEGACY  || { echo "SKIP: build failed"; exit 0; }
cmp -s "$T/peer_const.elf"  "$T/peer.elf" && { echo "FAIL: -DINCARN_CONSTANT did not land"; exit 1; }
cmp -s "$T/peer_legacy.elf" "$T/peer.elf" && { echo "FAIL: -DBEACON_LEGACY did not land"; exit 1; }

# Watch only 0x88B6, so a REBOOT line for it is unambiguous.
LOADER="-device loader,addr=0x20007f00,data=0x52454550,data-len=4
        -device loader,addr=0x20007f04,data=1,data-len=4
        -device loader,addr=0x20007f08,data=0x88b6,data-len=4"

rc=0

# ── One reboot experiment: bring observer up, let it lock onto the peer, KILL the peer,
#    RESTART it, and report what the observer said about the restart. ──
run_reboot() {  # <peer.elf> <logfile>
    local peerelf="$1" log="$2"
    local M="230.0.0.$(( (RANDOM % 200) + 20 )):$(( (RANDOM % 20000) + 20000 ))"
    : > "$log"; : > "$log.peer"
    OBS=$(setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -nic socket,mcast=$M,model=mcxn-enet,mac=54:27:8d:00:00:01 $LOADER \
        -kernel "$T/obs.elf" -no-reboot >"$log" 2>/dev/null & echo $!)
    # boot 1 of the peer -- its own stdout is captured so we can PROVE its nonce is per-boot.
    PEER=$(setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -nic socket,mcast=$M,model=mcxn-enet,mac=54:27:8d:00:00:02 \
        -kernel "$peerelf" -no-reboot >>"$log.peer" 2>/dev/null & echo $!)
    # observer must LOCK ON (record the peer's boot-1 incarnation) before we reboot it.
    wait_for "$log" 'rx: ethertype 0x88b6' 1 30 || echo "   (peer boot-1 never seen)"
    sleep 1                          # let a few boot-1 sequences climb past 1
    kpg "$PEER"                      # power-cycle the peer
    sleep 1
    # boot 2: a fresh QEMU -> a fresh incarnation, sequence back to 1
    PEER=$(setsid "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -nic socket,mcast=$M,model=mcxn-enet,mac=54:27:8d:00:00:02 \
        -kernel "$peerelf" -no-reboot >>"$log.peer" 2>/dev/null & echo $!)
    # give the observer time to classify the restart (reboot OR replay, per the flavour)
    wait_for "$log" 'ENET-LAB3 REBOOT: peer 0x88b6|CORRUPT: PAYLOAD-REPLAY' 1 20 || true
    sleep 1
    kpg "$OBS" "$PEER"; OBS=0; PEER=0
}

# ── A: honest per-boot peer.  Restart => REBOOT, and NOT a replay. ──
echo "── A: honest peer (per-boot incarnation) is power-cycled ──"
run_reboot "$T/peer.elf" "$T/a.log"
a_reboot=$(grep -c 'ENET-LAB3 REBOOT: peer 0x88b6' "$T/a.log"); a_reboot=${a_reboot:-0}
a_replay=$(grep -c 'CORRUPT: PAYLOAD-REPLAY' "$T/a.log"); a_replay=${a_replay:-0}
# ⭐ THE PEER'S OWN TWO BOOTS MUST DRAW TWO DIFFERENT NONCES.  If the ELS DTRNG were still
#   constant-seeded, both boots would print the same incarnation and the whole feature
#   would be theatre.  (tr -d '\r': the M33 prints CRLF; without it, sort sees the \r.)
a_incs=$(grep 'ENET-LAB3 incarnation' "$T/a.log.peer" | tr -d '\r' | grep -oE '0x[0-9a-f]{8}' | sort -u | wc -l)
echo "   reboots detected: $a_reboot   replays (must be 0): $a_replay   peer's distinct per-boot nonces (must be 2): $a_incs"
[ "$a_reboot" -ge 1 ] || { echo "FAIL(A): an honest peer's restart was NOT recognised as a reboot"; rc=1; }
[ "$a_replay" -eq 0 ] || { echo "FAIL(A): an honest reboot was condemned as a REPLAY ($a_replay) -- the false positive the incarnation exists to kill"; rc=1; }
[ "$a_incs" -ge 2 ] || { echo "FAIL(A): the peer's two boots did not draw two distinct nonces ($a_incs) -- the DTRNG is not per-boot and the reboot detection is luck"; rc=1; }

# ── C: constant-nonce control.  The SAME restart must now be condemned as a replay, and
#    NOT seen as a reboot.  If the incarnation did nothing, A and C would look identical. ──
echo "── C: NEGATIVE CONTROL -- constant nonce (-DINCARN_CONSTANT) is power-cycled ──"
run_reboot "$T/peer_const.elf" "$T/c.log"
c_reboot=$(grep -c 'ENET-LAB3 REBOOT: peer 0x88b6' "$T/c.log"); c_reboot=${c_reboot:-0}
c_replay=$(grep -c 'CORRUPT: PAYLOAD-REPLAY' "$T/c.log"); c_replay=${c_replay:-0}
# The mirror of A's proof: this peer's two boots draw the SAME nonce -- exactly why the
# restart is indistinguishable from a replay.  1 distinct nonce is the whole cause.
c_incs=$(grep 'ENET-LAB3 incarnation' "$T/c.log.peer" | tr -d '\r' | grep -oE '0x[0-9a-f]{8}' | sort -u | wc -l)
echo "   reboots detected (must be 0): $c_reboot   replays caught (must be >=1): $c_replay   peer's distinct nonces (must be 1): $c_incs"
[ "$c_replay" -ge 1 ] || { echo "FAIL(C): with a CONSTANT nonce the restart was NOT condemned as a replay -- then the per-boot nonce is doing nothing and case A proves nothing"; rc=1; }
[ "$c_reboot" -eq 0 ] || { echo "FAIL(C): a constant nonce was somehow read as a NEW incarnation ($c_reboot)"; rc=1; }
[ "$c_incs" -eq 1 ] || { echo "FAIL(C): the constant-nonce control drew $c_incs distinct nonces, not 1 -- the control is not actually pinning the value"; rc=1; }

# ── D: legacy peer (no incarnation).  Seen, never corrupt, no freshness verdict. ──
echo "── D: legacy peer (-DBEACON_LEGACY, sentinel 0x5a5a5a5a) is power-cycled ──"
run_reboot "$T/peer_legacy.elf" "$T/d.log"
d_seen=$(grep -c 'rx: ethertype 0x88b6' "$T/d.log"); d_seen=${d_seen:-0}
d_corrupt=$(grep -c 'CORRUPT:' "$T/d.log"); d_corrupt=${d_corrupt:-0}
d_reboot=$(grep -c 'ENET-LAB3 REBOOT' "$T/d.log"); d_reboot=${d_reboot:-0}
echo "   legacy peer seen: $d_seen   corrupt (must be 0): $d_corrupt   reboot/replay verdicts (must be 0): $d_reboot"
[ "$d_seen" -ge 1 ] || { echo "FAIL(D): a well-formed legacy peer was not even seen"; rc=1; }
[ "$d_corrupt" -eq 0 ] || { echo "FAIL(D): a legacy peer was called CORRUPT ($d_corrupt) -- freshness UNVERIFIABLE must not mean RED"; rc=1; }
[ "$d_reboot" -eq 0 ] || { echo "FAIL(D): a legacy peer got a reboot verdict ($d_reboot) it has no incarnation to earn"; rc=1; }

[ $rc -eq 0 ] && echo "PASS: a reboot is distinguished from a replay by the per-boot incarnation, and a constant nonce brings the bug straight back"
exit $rc
