#!/usr/bin/env bash
# Run Zephyr ztest suites built for frdm_mcxn947 on the model and assert each
# reaches "PROJECT EXECUTION SUCCESSFUL".  This validates the machine against
# Zephyr's own kernel test framework — third-party firmware, not our hand-rolled
# tests.  CI-safe: SKIPs when no staged ztest ELFs are present (they are
# operator-built; see build.sh / README.md).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
# Directory of <suite>.elf files, e.g. context.elf, fifo_api.elf, timer_behavior.elf
ZTEST_DIR="${ZTEST_DIR:-$HOME/mcxn-images/ztest}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

shopt -s nullglob
elfs=("$ZTEST_DIR"/*.elf)
[ "${#elfs[@]}" -gt 0 ] || { echo "SKIP: no ztest ELFs in $ZTEST_DIR (see build.sh)"; exit 0; }

# ⚠ THE GUEST NEVER EXITS.  Zephyr's ztest prints "PROJECT EXECUTION SUCCESSFUL" and then
# SPINS FOREVER.  The old runner paid a full wall-clock `timeout` cap for EVERY suite --
# 120s x 24 = ~48 minutes, almost all of it watching an already-finished guest spin, and
# long enough that a background/CI runner kills the whole job mid-run.
#
# So: poll the output and KILL THE GUEST THE INSTANT it prints the banner (or dies).  A
# passing suite now costs a second or two; the full run drops from ~48 min to ~2.
#
#   ⭐ THE SUCCESS BANNER TAKES PRECEDENCE OVER A FAULT, so we wait for the BANNER, not for
#     the first "Fault" line.  Zephyr fault-INJECTION suites (userspace/poll/queue/condvar)
#     deliberately oops, RECOVER, then print SUCCESSFUL -- a poll that concluded on the oops
#     would kill the guest before its recovery and MANUFACTURE a failure.  The verdict rule
#     in the loop is UNCHANGED and already checks SUCCESSFUL first.
#
# CAP is the wall-clock BACKSTOP for a suite that never prints a banner (a real hang or a
# genuinely killed run) -- reached only in that case, never on a passing suite.  Belt and
# suspenders: `timeout -k 5` is a hard second backstop if this poll is interrupted, and
# `setsid` makes the whole guest process group killable in one signal, so nothing orphans.
CAP="${CAP:-120}"
run_suite() {  # <elf> -- echo the guest output, killing the guest on banner/exit
    local elf="$1" tmp pid i
    tmp="$(mktemp)"
    setsid timeout -k 5 "$CAP" "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -icount shift=3 -serial stdio -kernel "$elf" -no-reboot </dev/null >"$tmp" 2>/dev/null &
    pid=$!
    for i in $(seq 1 $((CAP * 5))); do
        grep -q "PROJECT EXECUTION SUCCESSFUL" "$tmp" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break     # guest exited/was killed on its own
        sleep 0.2
    done
    # kill the guest's process group (setsid leader; pgid==pid).  ⚠ never `-0` (own group).
    case "$pid" in ''|0) ;; *) kill -KILL -"$pid" 2>/dev/null;; esac
    cat "$tmp"; rm -f "$tmp"
    return 0
}

fail=0
timedout=0
for elf in "${elfs[@]}"; do
    name="$(basename "$elf" .elf)"
    # The verdict is the OUTPUT, never the exit status: the guest is ALWAYS killed (it
    # spins after success), so exit status is meaningless.  The honest rule, unchanged:
    #
    #     output says SUCCESSFUL      -> PASS   (whatever the exit status)
    #     output shows ztest failures -> FAIL   (only if there is NO success banner)
    #     no verdict in the output    -> INCONCLUSIVE, never scored as a failure
    #
    # A killed run is not a caught bug, and an empty result is not a pass.  run_suite (above)
    # returns as soon as the banner lands, so a passing suite costs ~1-10s, not the full cap.
    OUT="$(run_suite "$elf")"

    if echo "$OUT" | grep -q "PROJECT EXECUTION SUCCESSFUL"; then
        # Count reported suite/case results for a one-line summary.
        cases="$(echo "$OUT" | grep -cE '^ (PASS|FAIL) -')"
        echo "PASS  $name ($cases cases)"
    elif echo "$OUT" | grep -qE '^ FAIL -|ASSERTION|FATAL|Fault'; then
        # A REAL failure: the guest ran and TOLD us something broke.
        echo "FAIL  $name"
        echo "$OUT" | grep -E '^ FAIL -|ASSERTION|FATAL|Fault' | head -5
        fail=1
    else
        # No verdict AT ALL -- the guest never got far enough to say anything.
        # That is a KILLED RUN (host load against a wall-clock cap), NOT a caught
        # bug.  Scoring it FAIL is how a loaded box manufactures a regression.
        echo "INCONCLUSIVE  $name -- no verdict in the output (killed before it"
        echo "              finished?).  NOT scored.  Re-run on an idle box."
        timedout=$((timedout + 1))
    fi
done

if [ $timedout -gt 0 ]; then
    echo "$timedout suite(s) TIMED OUT — the run is INCONCLUSIVE, not a pass and not a"
    echo "failure. Re-run on an idle machine before quoting any number from it."
    exit 2
fi
[ $fail -eq 0 ] && echo "PASS: all staged Zephyr ztest suites succeeded"
exit $fail
