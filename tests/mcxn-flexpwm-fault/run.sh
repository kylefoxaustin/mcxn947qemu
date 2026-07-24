#!/usr/bin/env bash
# MCXN947 eFlexPWM FAULT protection + NVIC interrupt (operator-driven).
#
# The guest configures the FAULT0 input and waits; the harness toggles the "fault-input" QOM
# property on each printed marker to drive the pin, exercising three axes:
#   ARMED-A      -> fault=1  : active-high fault -> FFLAG latches, FFPIN mirrors, IRQ 113 taken;
#                              a W1C while asserted is refused (the clear interlock).
#   ARMED-A-CLR  -> fault=0  : deassert so the guest's W1C can finally clear FFLAG.
#   ARMED-B      -> fault=1  : with FIE=0, FFLAG still latches but NO interrupt (FIE gate).
#   ARMED-B-CLR  -> fault=0  : deassert + clear.
# Phase C (FLVL polarity) holds the input LOW and toggles only FLVL, so it needs no injection.
# Before the fault path was wired, none of this happened: the fault registers were inert.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexpwm-fault.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal, sys

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-flexpwm-fault-%d.sock" % os.getpid()
if os.path.exists(sock):
    os.remove(sock)
p = subprocess.Popen(
    [qemu, "-M", "frdm-mcxn947", "-display", "none", "-monitor", "none",
     "-serial", "stdio", "-kernel", elf, "-no-reboot",
     "-qmp", "unix:%s,server,nowait" % sock],
    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

for _ in range(100):
    if os.path.exists(sock):
        break
    time.sleep(0.05)
qs = socket.socket(socket.AF_UNIX); qs.connect(sock); qf = qs.makefile('rw')
qf.readline()
qf.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); qf.flush()
qf.readline()

def drive(level):
    qf.write(json.dumps({"execute": "qom-set", "arguments": {
        "path": "/machine/soc/pwm0",
        "property": "fault-input", "value": bool(level)}}) + "\n")
    qf.flush(); qf.readline()

# Each marker -> the fault-input level the harness drives next.
INJECT = {
    "ARMED-A":     1,
    "ARMED-A-CLR": 0,
    "ARMED-B":     1,
    "ARMED-B-CLR": 0,
}   # phase C toggles only FLVL with the input held LOW -- no operator injection needed

out = []
deadline = time.time() + 14
os.set_blocking(p.stdout.fileno(), False)
while time.time() < deadline:
    line = p.stdout.readline()
    if line:
        s = line.decode(errors="replace")
        out.append(s)
        tok = s.strip().split()          # exact last-token match: "ARMED-A" != "ARMED-A-CLR"
        if tok and tok[-1] in INJECT:
            drive(INJECT[tok[-1]])
        if "FAULT PASS" in s or "FAULT FAIL" in s:
            break
    else:
        time.sleep(0.02)

p.send_signal(signal.SIGTERM)
try:
    p.wait(timeout=3)
except Exception:
    p.kill()
if os.path.exists(sock):
    os.remove(sock)
sys.stdout.write("".join(out))
PY
)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "FAULT PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
