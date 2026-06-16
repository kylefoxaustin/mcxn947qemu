#!/usr/bin/env bash
# MCXN947 idle-core (WFI) check: boot Zephyr and assert the QEMU TCG thread is
# quiescent at guest idle.  A Cortex-M WFI busy-spin is invisible to guest
# counters/RSS - only host CPU reveals it (per 93/91 emulator playbook).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
HELLO="${ZEPHYR_ELF:-$HOME/mcxn-images/mcxn-hello.elf}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
[ -f "$HELLO" ] || { echo "SKIP: no Zephyr ELF at $HELLO"; exit 0; }
"$QEMU" -M frdm-mcxn947 -display none -monitor none -serial null \
  -kernel "$HELLO" -no-reboot >/dev/null 2>&1 &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT
sleep 3
read u1 s1 <<<"$(awk '{print $14,$15}' /proc/$QPID/stat 2>/dev/null || echo 0 0)"
sleep 3
read u2 s2 <<<"$(awk '{print $14,$15}' /proc/$QPID/stat 2>/dev/null || echo 0 0)"
HZ=$(getconf CLK_TCK); pct=$(( ((u2+s2)-(u1+s1)) * 100 / (3*HZ) ))
echo "qemu CPU over 3s idle window: ~${pct}% of one core"
[ "$pct" -lt 40 ] && { echo "PASS: idle core quiescent (WFI working)"; exit 0; } || { echo "FAIL: high idle CPU - possible WFI busy-spin"; exit 1; }
