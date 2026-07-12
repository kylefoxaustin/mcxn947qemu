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
echo "── build OK: the mutation is genuinely in the binary"

echo "── running $TESTDIR (it MUST now fail)"
if bash "$TESTDIR/run.sh" >/dev/null 2>&1; then
    echo
    echo "❌ THE TEST STILL PASSES WITH THE MODEL BROKEN."
    echo "   It CANNOT FAIL in this dimension — it is decoration, and you could"
    echo "   not have told by reading it. Fix the TEST, not the model."
    exit 1
fi

echo
echo "✅ the test caught the mutation — it can fail, and does."
