#!/usr/bin/env bash
#
# Run every mcxn-* suite against ONE PINNED BINARY.
#
# WHY THIS IS A SCRIPT AND NOT A COMMAND I RETYPE.  I have invalidated a
# "definitive" suite run FOUR times by rebuilding the tree while it was running --
# the run half-tests the old binary and half the new one, and NOTHING SAYS SO.
#
# I pinned the md5 at start and end to catch it.  That guard is NOT ENOUGH, and
# finding out why is the point of this comment:
#
#   ⭐ tests/mutate.sh RESTORES THE SOURCE AND REBUILDS.  The restored build is
#      BYTE-IDENTICAL.  So a full mutate-and-restore cycle -- during which the suite
#      was running against a DELIBERATELY BROKEN binary -- leaves start-md5 ==
#      end-md5.  THE GUARD I BUILT TO CATCH THIS EXACT MISTAKE CANNOT SEE IT.
#
# A hash proves the binary is the same at two instants.  It says NOTHING about the
# interval between them, which is the only part the suite actually ran in.  So pin
# the MTIME too: any rebuild moves it, even one that reproduces the same bytes.
#
#     "A check that did not RUN looks exactly like a check that found NOTHING."
#     And a check that CANNOT SEE the event looks exactly like one that saw it and
#     was happy.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

pin() { printf '%s %s' "$(md5sum "$QEMU" | cut -c1-12)" "$(stat -c %Y "$QEMU")"; }
PIN0="$(pin)"
echo "binary pin: $PIN0  (md5 + mtime)"

pass=0; fail=0; failed=""
for d in tests/mcxn-*/; do
    n="$(basename "$d")"
    [ -x "$d/run.sh" ] || continue
    [ "$n" = "mcxn-ztest" ] && continue          # run separately; see its own notes
    if timeout 300 bash "$d/run.sh" >/dev/null 2>&1; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1)); failed="$failed $n"
    fi
done

echo "════ mcxn suite: $pass passed, $fail failed ════"
[ -n "$failed" ] && echo "FAILED:$failed"

PIN1="$(pin)"
if [ "$PIN0" != "$PIN1" ]; then
    echo
    echo "⚠️  THE BINARY CHANGED UNDER THIS RUN ($PIN0 -> $PIN1)."
    echo "    Part of this suite tested a different build.  THE RESULT ABOVE IS VOID"
    echo "    -- not 'probably fine'.  Rebuild, then re-run with nothing else going on."
    exit 2
fi
exit $([ $fail -eq 0 ] && echo 0 || echo 1)
