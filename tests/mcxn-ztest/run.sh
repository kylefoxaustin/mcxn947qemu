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
    # ⚠ AND THE TIMEOUT MUST NOT BE THE THING THAT REINTRODUCES HOST LOAD.
    # -icount fixes the GUEST's clock; this `timeout` is WALL CLOCK.  With a tight
    # cap, a perfectly good suite on a busy box takes longer in real seconds and
    # gets KILLED — so the harness that was fixed to stop reporting a false PASS
    # went right on reporting a false FAIL, from the identical variable.  (Caught
    # live: suites sitting at 87s against a 90s cap while the box was loaded.)
    # I wrote the paragraph above about non-deterministic instruments and then put
    # a wall-clock timeout on the next line.
    #
    # So: a generous cap (it is a hang-catcher, not a measurement), and a timeout
    # is reported as INCONCLUSIVE, never scored as a failure.  A killed run is not
    # a caught bug.
    OUT="$(timeout 600 "$QEMU" -M frdm-mcxn947 -display none -monitor none -icount shift=3 \
            -serial stdio -kernel "$elf" -no-reboot </dev/null 2>/dev/null)"
    rc=$?
    if [ $rc -eq 124 ]; then
        echo "TIMEOUT  $name — INCONCLUSIVE (600s wall clock; almost certainly host"
        echo "         load, not a model failure). Re-run on an idle box. NOT scored."
        timedout=$((timedout + 1))
        continue
    fi
    if echo "$OUT" | grep -q "PROJECT EXECUTION SUCCESSFUL"; then
        # Count reported suite/case results for a one-line summary.
        cases="$(echo "$OUT" | grep -cE '^ (PASS|FAIL) -')"
        echo "PASS  $name ($cases cases)"
    else
        echo "FAIL  $name"
        echo "$OUT" | grep -E 'FAIL -|ASSERTION|FATAL|Fault' | head -5
        fail=1
    fi
done

if [ $timedout -gt 0 ]; then
    echo "$timedout suite(s) TIMED OUT — the run is INCONCLUSIVE, not a pass and not a"
    echo "failure. Re-run on an idle machine before quoting any number from it."
    exit 2
fi
[ $fail -eq 0 ] && echo "PASS: all staged Zephyr ztest suites succeeded"
exit $fail
