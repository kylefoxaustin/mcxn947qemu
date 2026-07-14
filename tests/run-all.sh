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

#
# ⛔ TWO DIFFERENT QUESTIONS, AND FOR MONTHS I ONLY ASKED ONE.
#
#   the PIN (below) asks:  "did the binary CHANGE **DURING** the run?"
#   this gate asks:        "was the binary CURRENT **WHEN** the run started?"
#
# The pin catches a `ninja` racing the suite -- which has invalidated three separate
# "definitive" runs.  It is completely blind to the OTHER stale-binary failure: SOURCE
# EDITED, BUILD FORGOTTEN (or a build that FAILED), so the suite dutifully tests the
# binary from before the change and reports green on code that was never compiled.
#
# I hit this twice in one night.  93emulator hit it INSIDE THE HOUR they told this bus
# they had internalised the rule, and named the reason it survives:
#
#   ⭐ A BROKEN BUILD PRODUCES A QUIET, PLAUSIBLE, WRONG NUMBER -- AND
#     "MY NEW ASSERTION FOUND NOTHING" IS A VERY COMFORTABLE THING TO BELIEVE.
#     The wrong answer is the one you were hoping for.  (rt1180emulator)
#
# ⭐ NEVER TEST A BINARY YOU DID NOT JUST BUILD.  Reading a rule is not holding it, so
#   the rule is a BRANCH now and not a comment.
#
STALE="$(find hw include/hw -name 'mcxn_*.[ch]' -newer "$QEMU" -print -quit 2>/dev/null)"
if [ -n "$STALE" ]; then
    echo "REFUSING TO RUN: $QEMU is OLDER than the model source it claims to test."
    echo "  newer than the binary: $STALE"
    echo "  (a suite that tests a stale binary reports green on code that was never"
    echo "   compiled.  Run 'ninja -C build' first.)"
    exit 2
fi

#
# ⭐ THE ENVIRONMENT IS PART OF THE MEASUREMENT.  A NUMBER WITHOUT ITS CENSUS IS NOT A
#   NUMBER.                                                              (qualcomm)
#
# I burned 30 CORE-HOURS of this box with 8 orphaned QEMUs -- two from a demo I ran by
# hand and never killed, six leaked by `timeout N` (which sends SIGTERM and then WAITS
# FOREVER if the child ignores it; a QEMU spinning under -icount does exactly that).
#
# And the reason I never questioned the resulting slowness is the sharpest thing anyone
# said this week:
#
#   ⭐ "WORSE RESULTS ARE THE ONES WE ARE LEAST LIKELY TO CHALLENGE, BECAUSE
#      DISAPPOINTING NUMBERS FEEL LIKE HONESTY."
#
# A contaminated box does not produce obviously-broken results.  It produces plausible,
# disappointing, WRONG ones -- and you thank it for its candour.  My ENET lab test failed
# twice; I found a real bug the first time and blamed "load" the second.  The load was me.
#
# So: the census prints WITH the verdict, every run.  Timing tests here use -icount and
# are immune, and the lab tests POLL rather than sleep -- but a reader deserves to know
# what else was on the machine, and a HOT TENANT WITH NO OWNER is a finding.
#
# ⭐ AND THE CENSUS MUST NAME ITS OWN BLIND SPOT.                        (95emulator)
#   A CPU-SORTED CENSUS IS STRUCTURALLY BLIND TO THE CORPSE I ACTUALLY HAD.  I first wrote
#   pass 2 as "qemu-system reparented to PID 1 (PPID==1)" -- and it caught ZERO of my eight
#   leaked QEMUs, because the `timeout N` leak DOES NOT PRODUCE PID-1 ORPHANS:
#
#     six of mine were children of a `timeout` STILL BLOCKED WAITING (it SIGTERM'd a QEMU
#     that ignores SIGTERM under -icount, so `timeout` never returns -- parent ALIVE, child
#     never reparented).  The other two hung off `systemd --user`.  A predicate keyed on
#     parentage is blind, BY CONSTRUCTION, to the exact bug it was written to find.
#
#   The blind-spot-free predicate is not WHO the parent is -- it is AGE.  No suite test runs
#   a QEMU longer than its timeout (the largest is 240s).  So a `qemu-system-*` alive past
#   ~10min is a leak, whatever its parent.  That catches all eight; PPID==1 caught none.
#
#     1. hot long-lived tenants (any process >50%% cpu, >10min) -- contention right now.
#     2. leaked QEMUs (qemu-system-* older than any legitimate test run), at ANY cpu --
#        the pass the hot-list is blind to when a leak idles below the cpu threshold.
#
QEMU_LEAK_AGE_S=600     # > longest suite timeout (240s) by a wide margin; no real run lives this long
tenant_census() {
    local hot leaks
    hot="$(ps -eo pcpu,etimes,comm --sort=-pcpu 2>/dev/null |
           awk 'NR>1 && $1 > 50 && $2 > 600 {n++} END {print n+0}')"
    printf 'tenant census: %s hot long-lived process(es) (>50%%%% cpu, >10min) · load %s · %s cores\n' \
        "$hot" "$(cut -d' ' -f1 /proc/loadavg 2>/dev/null)" "$(nproc 2>/dev/null)"
    if [ "${hot:-0}" -gt 0 ]; then
        # detail filter MUST index the detail columns (pid pcpu etimes comm), not the count's.
        ps -eo pid,pcpu,etimes,comm --sort=-pcpu 2>/dev/null |
          awk 'NR>1 && $2+0 > 50 && $3+0 > 600 {printf "   ⚠ pid=%s %s%% %.1fh %s\n", $1, $2, $3/3600, $4}' |
          head -5
        echo "   ⚠ this run shared the machine.  -icount tests are immune; wall-clock ones are not."
    fi
    # Pass 2: leaked QEMUs by AGE, at ANY cpu, whatever the parent -- catches the stuck-timeout leak.
    leaks="$(ps -eo etimes,comm 2>/dev/null | awk -v a="$QEMU_LEAK_AGE_S" '$1+0 > a && $2 ~ /qemu-system/ {n++} END {print n+0}')"
    printf 'orphan census: %s leaked qemu-system-* (alive >%.0fmin, any cpu -- older than any legitimate test run)\n' \
        "$leaks" "$((QEMU_LEAK_AGE_S/60))"
    if [ "${leaks:-0}" -gt 0 ]; then
        ps -eo pid,ppid,pcpu,etimes,comm 2>/dev/null |
          awk -v a="$QEMU_LEAK_AGE_S" '$4+0 > a && $5 ~ /qemu-system/ {printf "   ☠ leak pid=%s ppid=%s %s%% %.1fh %s\n", $1, $2, $3, $4/3600, $5}' |
          head -8
        echo "   ☠ these outlived every test timeout -- reap by exact PID (never pkill -f: siblings share this box)."
    fi
}
tenant_census

pin() { printf '%s %s' "$(md5sum "$QEMU" | cut -c1-12)" "$(stat -c %Y "$QEMU")"; }
PIN0="$(pin)"
echo "binary pin: $PIN0  (md5 + mtime, and NEWER than every mcxn_* source)"

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
