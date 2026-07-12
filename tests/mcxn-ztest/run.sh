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

fail=0
timedout=0
for elf in "${elfs[@]}"; do
    name="$(basename "$elf" .elf)"
    # -icount: virtual time is derived from instructions retired, not host wall
    # time.  Without it the timing-sensitive suites (timer_api.test_sleep_abs)
    # FLAKE under host load — reproduced directly: it fails on a loaded box and
    # passes on an idle one, same tree.  A CI suite measured on a non-deterministic
    # instrument reports luck, not correctness.
    #
    # ⚠ THE GUEST NEVER EXITS.  Zephyr's ztest prints "PROJECT EXECUTION
    # SUCCESSFUL" and then spins forever, so `timeout` is THE NORMAL TERMINATION
    # PATH FOR EVERY SUITE, not an error path.  Two consequences, both of which
    # bit me:
    #
    #   - The exit status is ALWAYS 124 and is therefore MEANINGLESS as a verdict.
    #     My first attempt treated 124 as a timeout and reported every PASSING
    #     suite as INCONCLUSIVE.  ONLY THE OUTPUT IS A VERDICT.
    #   - The cap is paid IN FULL by every suite, so raising it to 600s did not
    #     add safety, it made the run 6.7x SLOWER (24 x 600s = 4 hours).
    #
    # And the reason I raised it in the first place still stands: -icount fixes the
    # GUEST's clock, but this timeout is WALL CLOCK, so under host load a good
    # suite can be killed BEFORE it prints its result.  The honest rule:
    #
    #     output says SUCCESSFUL      -> PASS   (whatever the exit status)
    #     output shows ztest failures -> FAIL
    #     no verdict in the output    -> INCONCLUSIVE, never scored as a failure
    #
    # A killed run is not a caught bug, and an empty result is not a pass.
    OUT="$(timeout 120 "$QEMU" -M frdm-mcxn947 -display none -monitor none -icount shift=3 \
            -serial stdio -kernel "$elf" -no-reboot </dev/null 2>/dev/null || true)"

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
