#!/usr/bin/env bash
# MCXN947 soak harness (93/91-emulator playbook style).
#
# Loops the whole per-feature test suite for N cycles, asserting CORRECTNESS
# every cycle (each test self-verifies and prints PASS), and tracks health:
#   - incidents: any non-PASS/non-SKIP result -> logged to incidents/, counted,
#     and the soak CONTINUES (never aborts on one failure).
#   - per-test pass counters: every non-skipped test MUST keep climbing; a
#     stuck counter = a starved or hung path.
#   - RSS-leak watch: a long-running Zephyr-networking instance is held up for
#     the whole soak and its VmRSS sampled each cycle; growth flags a leak.
#   - idle-core (WFI) re-checked via the mcxn-idle suite each cycle.
#
# Usage: soak.sh [CYCLES]   (default 3 = quick smoke; pass e.g. 100 for a real
# soak, or set SOAK_MINUTES=N to run by wall-clock instead).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
CYCLES="${1:-3}"
SOAK_MINUTES="${SOAK_MINUTES:-0}"
INC="$HERE/incidents"; mkdir -p "$INC"; rm -f "$INC"/*.log 2>/dev/null
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }

# Tests to loop (exclude the soak itself and the slow whole-suite metas).
mapfile -t TESTS < <(cd "$ROOT/tests" && ls -d mcxn-*/ | sed 's,/,,' | grep -vE 'mcxn-soak')

# Hold a long-running networking guest for the RSS-leak watch (optional).
NET_ELF="${ZEPHYR_ELF:-$HOME/mcxn-images/mcxn-hello.elf}"
LEAKPID=""
if [ -f "$NET_ELF" ]; then
  "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial null \
    -nic user,model=mcxn-enet -kernel "$NET_ELF" -no-reboot >/dev/null 2>&1 &
  LEAKPID=$!
fi
cleanup(){ [ -n "$LEAKPID" ] && kill "$LEAKPID" 2>/dev/null; }
trap cleanup EXIT

declare -A passc skipc
boots=0; incidents=0; rss0=0; rssN=0
start=$(date +%s)
c=0
while :; do
  c=$((c+1))
  for t in "${TESTS[@]}"; do
    boots=$((boots+1))
    out="$(timeout -s KILL 60 bash "$ROOT/tests/$t/run.sh" 2>&1 || true)"
    if echo "$out" | grep -qaE '\bPASS\b'; then passc[$t]=$(( ${passc[$t]:-0} + 1 ))
    elif echo "$out" | grep -qaiE 'SKIP'; then skipc[$t]=$(( ${skipc[$t]:-0} + 1 ))
    else incidents=$((incidents+1)); echo "$out" > "$INC/cycle${c}_${t}.log"; fi
  done
  if [ -n "$LEAKPID" ] && [ -r "/proc/$LEAKPID/status" ]; then
    r=$(awk '/VmRSS/{print $2}' /proc/$LEAKPID/status)
    [ "$c" = 1 ] && rss0=$r; rssN=$r
  fi
  echo "cycle $c: boots=$boots incidents=$incidents net_rss=${rssN}kB"
  if [ "$SOAK_MINUTES" -gt 0 ]; then
    [ $(( ($(date +%s) - start) / 60 )) -ge "$SOAK_MINUTES" ] && break
  else
    [ "$c" -ge "$CYCLES" ] && break
  fi
done

echo "==================== SOAK SUMMARY ===================="
echo "cycles=$c boots=$boots incidents=$incidents"
[ "$rss0" -gt 0 ] && echo "net guest RSS: ${rss0}kB -> ${rssN}kB (Δ$(( rssN - rss0 ))kB over soak)"
# Counter-climb check: every non-skipped test must have passed every cycle.
stuck=0
for t in "${TESTS[@]}"; do
  p=${passc[$t]:-0}; s=${skipc[$t]:-0}
  [ "$s" -ge "$c" ] && continue        # always skipped (missing dep) - ignore
  if [ "$p" -lt "$c" ]; then echo "  STUCK/FLAKY: $t passed $p/$c"; stuck=$((stuck+1)); fi
done
# Leak check: flag >25% RSS growth.
leak=0
if [ "$rss0" -gt 0 ] && [ "$rssN" -gt $(( rss0 * 5 / 4 )) ]; then echo "  LEAK SUSPECT: net RSS grew >25%"; leak=1; fi
if [ "$incidents" -eq 0 ] && [ "$stuck" -eq 0 ] && [ "$leak" -eq 0 ]; then
  echo "PASS: $boots boots, 0 incidents, all counters climbing, RSS flat"; exit 0
else
  echo "FAIL: incidents=$incidents stuck=$stuck leak=$leak (see $INC/)"; exit 1
fi
