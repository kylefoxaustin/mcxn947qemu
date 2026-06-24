#!/usr/bin/env bash
# MCXN947 BSP-matrix boot-smoke: boot one validated firmware entry on the model
# and assert its banner marker. The MCX is bare-metal — boot is just
# `-M frdm-mcxn947 -kernel <elf>` (no -dtb/-initrd/-smp). This is the cheap,
# re-runnable regression; run it on boot-affecting model changes + when adding a
# BSP entry. Entries are defined in docs/validation/bsp-matrix.yaml.
#
#   tests/bsp-matrix/run.sh [label]
#   ELF=/path/to.elf MARKER="..." tests/bsp-matrix/run.sh <label>
#
# Zephyr ELFs are staged (Apache-2.0). The MCUXpresso ELF is NOT redistributable
# (operator-built via tests/mcxn-mcuxpresso/build.sh) — this SKIPS cleanly if the
# entry's artifact is absent, like the other MCUXpresso tests.
set -u
REPO=$(cd "$(dirname "$0")/../.." && pwd)
QEMU=${QEMU:-$REPO/build/qemu-system-arm}
LABEL=${1:-zephyr-v4.4}
TIMEOUT=${TIMEOUT:-15}

# Default artifact + marker per known label (override with ELF= / MARKER=).
case "$LABEL" in
  zephyr-v4.4|zephyr-mainline)
    ELF=${ELF:-$HOME/mcxn-images/mcxn-hello.elf}
    MARKER=${MARKER:-"Hello World! frdm_mcxn947"} ;;
  mcuxpresso-sdk-2.16|mcuxpresso-latest)
    ELF=${ELF:-$HOME/mcux-build/mcux-hello.elf}
    MARKER=${MARKER:-"MCUXPRESSO-SDK-PASS"} ;;
  *)
    : "${ELF:?unknown label '$LABEL' — pass ELF= and MARKER=}" "${MARKER:?need MARKER=}" ;;
esac

[ -x "$QEMU" ] || { echo "SKIP: qemu not built ($QEMU)"; exit 0; }
[ -f "$ELF" ]  || { echo "SKIP: $LABEL artifact not staged ($ELF) — see docs/validation/bsp-matrix.yaml"; exit 0; }

LOG=$(mktemp /tmp/mcx-bsp-smoke.XXXX.log); trap 'rm -f "$LOG"' EXIT
echo "== MCX BSP-matrix boot-smoke: $LABEL =="
echo "   elf=$(basename "$ELF")  marker=\"$MARKER\""
timeout -s KILL "$TIMEOUT" "$QEMU" -M frdm-mcxn947 -display none -monitor none \
  -serial "file:$LOG" -kernel "$ELF" -no-reboot >/dev/null 2>&1

if grep -qF "$MARKER" "$LOG"; then
  echo "PASS: $LABEL booted to marker"
  exit 0
fi
echo "FAIL: $LABEL marker not seen"; echo "--- serial ---"; head -12 "$LOG"
exit 1
