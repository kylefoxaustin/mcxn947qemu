#!/usr/bin/env bash
#
# Mutation harness — break the model on purpose and PROVE the test notices.
#
# WHY THIS EXISTS AS A SCRIPT AND NOT AS A RULE I REMEMBER.  Doing this by hand
# has failed the SAME WAY TWICE:
#
#   Kill a line of code -> a variable becomes unused -> -Werror -> THE BUILD
#   FAILS -> the OLD binary is still sitting there -> the test runs against
#   unmutated code and "PASSES".
#
#   ⭐ A MUTATION THAT DID NOT COMPILE LOOKS EXACTLY LIKE A MUTATION THE TEST
#      FAILED TO CATCH.  Both are a green test next to a broken model.
#
# The second time it got me I had the lesson written in my own memory file and
# was actively looking for it — I just grepped ninja's OUTPUT for the filename
# instead of checking its EXIT STATUS.  A rule you have to remember at 3am is not
# a control.  So the check lives here, where it cannot be skipped:
#
#   1. the anchor MUST match          (else: the mutation was never applied)
#   2. the build MUST succeed         (else: you are testing the old binary)
#   3. the test MUST then FAIL        (else: the test cannot fail — it is
#                                      decoration, and you CANNOT tell by
#                                      reading it)
#   4. the file is ALWAYS restored and rebuilt, even on error.
#
# Usage:
#   tests/mutate.sh <file> <anchor-file> <replacement-file> <test-dir> [...]
#   tests/mutate.sh --inline <file> "<anchor>" "<replacement>" <test-dir>
#
# Example:
#   tests/mutate.sh --inline hw/misc/mcxn_dac.c \
#       'qemu_set_irq(s->dma_req, req);' \
#       'qemu_set_irq(s->dma_req, false);' \
#       tests/mcxn-dac-dma
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ "${1:-}" != "--inline" ] || [ $# -lt 5 ]; then
    sed -n '2,40p' "$0" | sed 's/^# \?//'
    exit 2
fi
FILE="$2"; ANCHOR="$3"; REPLACE="$4"; TESTDIR="$5"

[ -f "$FILE" ]           || { echo "no such file: $FILE"; exit 2; }
[ -x "$TESTDIR/run.sh" ] || { echo "no such test: $TESTDIR/run.sh"; exit 2; }

BAK="$(mktemp)"
cp "$FILE" "$BAK"

restore() {
    cp "$BAK" "$FILE"
    rm -f "$BAK"
    # Leave the tree exactly as we found it — a mutated binary left behind is
    # its own silent-wrong-answer generator.
    ( cd build && ninja ) >/dev/null 2>&1 || {
        echo "!!! FAILED TO REBUILD THE CLEAN TREE — DO NOT TRUST ANY LATER RUN"
        exit 3
    }
}
trap restore EXIT

echo "── mutating $FILE"
python3 - "$FILE" "$ANCHOR" "$REPLACE" <<'PY' || exit 4
import sys
path, anchor, replace = sys.argv[1], sys.argv[2], sys.argv[3]
src = open(path).read()
if anchor not in src:
    sys.stderr.write(
        "!!! ANCHOR NOT FOUND. The mutation was NEVER APPLIED, so a passing test\n"
        "    here would prove nothing at all. Fix the anchor.\n")
    sys.exit(1)
open(path, 'w').write(src.replace(anchor, replace, 1))
PY

#
# ⭐ GATE 4: THE MUTATION MUST REACH THE BINARY, NOT MERELY COMPILE.
#
# The line below used to read:  "build OK: the mutation is genuinely in the binary"
# -- and that was A CLAIM, NOT A CHECK.  A successful build proves the source still
# compiles.  It proves NOTHING about whether the output changed.
#
# I proved that on myself: I once "mutated" a source by appending a C COMMENT, the
# build succeeded, the gate correctly PASSED -- and I nearly filed the gate as
# ineffective.  A comment cannot change a binary.
#
#     ⭐ A MUTATION THAT CANNOT CHANGE THE OUTPUT CANNOT TEST A GATE ON THE OUTPUT.
#     ⭐ A MUTATION THAT DID NOT ARM TESTS NOTHING -- AND ITS GREEN IS
#       INDISTINGUISHABLE FROM A PASSING MODEL.                (95emulator, rt1180emulator)
#
# And rt1180 named the direction that makes it lethal: their un-armed mutation came
# back green -- "the model tolerates it!" -- and that is the result you do NOT go back
# and check, because it AGREES WITH YOU.  qualcomm: "the instrument fails in the
# direction that flatters the story you are already telling."
#
# So: hash the binary before and after.  If it did not move, the mutation is not in it,
# and every number that follows would be a verdict on the ORIGINAL model.
#
BIN=build/qemu-system-arm
BIN_BEFORE="$(md5sum "$BIN" 2>/dev/null | cut -d' ' -f1)"

echo "── rebuilding (the mutation MUST actually compile)"
if ! ( cd build && ninja ) > /tmp/mutate-build.$$ 2>&1; then
    echo "!!! THE MUTATION DID NOT COMPILE — a 'PASS' now would run the OLD binary"
    echo "    and would be MEANINGLESS. Rewrite the mutation so it builds"
    echo "    (e.g. keep variables used), then re-run."
    grep -E 'error:' /tmp/mutate-build.$$ | head -3
    rm -f /tmp/mutate-build.$$
    exit 5
fi
rm -f /tmp/mutate-build.$$

BIN_AFTER="$(md5sum "$BIN" 2>/dev/null | cut -d' ' -f1)"
if [ -n "$BIN_BEFORE" ] && [ "$BIN_BEFORE" = "$BIN_AFTER" ]; then
    echo
    echo "!!! THE MUTATION COMPILED BUT DID NOT CHANGE THE BINARY."
    echo "    md5 before = md5 after = $BIN_AFTER"
    echo
    echo "    So the run below would test the ORIGINAL model, and its verdict --"
    echo "    whichever way it fell -- would be about code you did not mutate."
    echo "    A MUTATION THAT CANNOT CHANGE THE OUTPUT CANNOT TEST A GATE ON THE OUTPUT."
    echo
    echo "    (Did you edit a comment?  Change a value nothing reads?  Touch a file"
    echo "     the build does not use?)  INCONCLUSIVE -- never a catch, never a miss."
    exit 7
fi
echo "── build OK, and the binary CHANGED ($BIN_BEFORE -> $BIN_AFTER):"
echo "   the mutation is genuinely in the binary -- CHECKED, not claimed"

echo "── running $TESTDIR (it MUST now fail — and FAIL is not the same as CRASH)"
OUT="$(bash "$TESTDIR/run.sh" 2>&1)"
rc=$?

#
# ⭐ GATE 3, AND THE HOLE IT USED TO HAVE.
#
# This used to be `if bash run.sh; then survived; else caught; fi` -- i.e. ANY
# non-zero exit was scored as "the test caught the mutation".  But a mutated build
# that makes QEMU crash, a run.sh that errors out, a missing toolchain, a SKIP:
# ALL EXIT NON-ZERO TOO.  Every one of them would have been reported as a catch.
#
#     AN EXIT-1 CRASH AND AN EXIT-1 REFUSAL ARE INDISTINGUISHABLE BY EXIT CODE
#     ALONE.  NEVER ASSERT "NON-ZERO".  ASSERT THE THING YOU MEANT.
#
# That is gate 2 -- "a check that did not RUN looks exactly like a check that
# found NOTHING" -- which I wrote INTO THIS FILE and then failed to apply to this
# file's own verdict.  ollama_95_neutron hit the identical bug in a verify script
# an hour later and named it: "I patched it where I MET it."
#
# So: the test must have RUN and REPORTED FAILURE.  A crash, a skip, or no verdict
# at all is INCONCLUSIVE -- never a catch.
#
if [ $rc -eq 0 ]; then
    echo
    echo "❌ THE TEST STILL PASSES WITH THE MODEL BROKEN."
    echo "   It CANNOT FAIL in this dimension — it is decoration, and you could"
    echo "   not have told by reading it. Fix the TEST, not the model."
    exit 1
fi

if echo "$OUT" | grep -q 'SKIP:'; then
    echo
    echo "⚠️  INCONCLUSIVE — the test SKIPPED (exit $rc).  It was never run, so this"
    echo "    proves NOTHING about whether it can catch the mutation."
    echo "$OUT" | grep 'SKIP:' | head -1
    exit 6
fi

if echo "$OUT" | grep -qiE 'segmentation fault|core dumped|command not found|unbound variable|syntax error|no such file|assertion.*failed.*qemu|qemu-system-arm: .*error'; then
    echo
    echo "⚠️  INCONCLUSIVE — the run CRASHED (exit $rc), it did not FAIL."
    echo "    A crash and a refusal are indistinguishable by exit code alone, and"
    echo "    scoring this as a catch would be a lie.  Fix the crash, then re-run."
    echo "$OUT" | grep -iE 'segmentation fault|command not found|unbound variable|syntax error|no such file' | head -2
    exit 7
fi

if ! echo "$OUT" | grep -qE '(^|[^A-Z])FAIL'; then
    echo
    echo "⚠️  INCONCLUSIVE — exit $rc but the test printed NO FAILURE VERDICT."
    echo "    It may have died before reaching its own checks.  An empty result is"
    echo "    not a pass, and it is not a catch either."
    echo "$OUT" | tail -3
    exit 8
fi

echo
echo "✅ the test caught the mutation — it RAN, and it REPORTED FAILURE."
