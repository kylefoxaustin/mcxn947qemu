#!/usr/bin/env bash
# MCXN947 REBOOT / system_reset regression (pre-upstream send-blocker class,
# flagged by 95emulator's cold-adversarial Fable gate: machines that release a
# secondary core are at risk of a 2nd-boot hang if the release does not re-run
# on reset).
#
# mcx releases cpu1 via a GUEST write to SYSCON CPUCTRL (not a one-shot
# machine_init_done notifier), and mcxn_syscon_reset() clears cpu1_running +
# restores CPUCTRL=held-in-reset, so on reboot cpu0 firmware re-releases cpu1.
# This test PROVES that end to end: boot -> both cores print their banner ->
# QMP system_reset -> BOTH banners must appear AGAIN.  A model that released
# cpu1 only once (or failed to clear cpu1_running on reset) would show "CPU1
# up" only once and hang the second boot.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
PY="${PY:-python3}"
ELF="$HERE/reboot.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
command -v "$PY" >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

SERIAL="$(mktemp)"; QMP="$(mktemp -u).sock"
trap 'rm -f "$SERIAL"' EXIT
# NOTE: no -no-reboot — that makes QEMU EXIT on system_reset instead of resetting.
timeout -k 5 25 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
    -serial "file:$SERIAL" -qmp "unix:$QMP,server=on,wait=off" -kernel "$ELF" &
QPID=$!

"$PY" - "$QMP" <<'PY'
import socket, json, sys, time
p = sys.argv[1]; s = None
for _ in range(60):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(p); break
    except OSError:
        s = None; time.sleep(0.1)
if not s:
    print("QMPERR"); sys.exit(0)
f = s.makefile("rwb", buffering=0)
def cmd(o):
    try:
        f.write((json.dumps(o) + "\n").encode())
    except BrokenPipeError:
        pass
f.readline(); cmd({"execute": "qmp_capabilities"}); f.readline()
time.sleep(1.5)                       # first boot: CPU0 up + CPU1 up
cmd({"execute": "system_reset"})
try:
    f.readline()
except Exception:
    pass
time.sleep(1.5)                       # second boot: CPU0 up + CPU1 up again
cmd({"execute": "quit"})
PY

sleep 0.3; kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
OUT="$(cat "$SERIAL" 2>/dev/null)"
c0="$(printf '%s\n' "$OUT" | grep -c 'CPU0 up')"
c1="$(printf '%s\n' "$OUT" | grep -c 'CPU1 up')"
echo "--- serial ---"; echo "$OUT"; echo "---------------"
echo "CPU0 up: $c0   CPU1 up: $c1  (each must be >= 2: booted before AND after system_reset)"
if [ "$c0" -ge 2 ] && [ "$c1" -ge 2 ]; then
    echo "PASS: both cores reached userspace again after system_reset (cpu1 re-released, no reboot hang)"
    exit 0
else
    echo "FAIL: a core did not come back after system_reset (reboot-hang send-blocker)"
    exit 1
fi
