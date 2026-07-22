#!/usr/bin/env bash
# MCXN947 eFlexPWM input-capture -> eDMA (operator-driven).
#
# The guest arms input-A capture on the RISING edge with capture DMA (DMAEN[CA0DE]) and a
# running counter, then waits.  The harness toggles the "capture-a-input" QOM property:
#   ARMED-RISE  -> input HIGH: a rising edge captures the counter into CVAL0 and the eDMA
#                  (mux source 39) moves it to memory.
#   ARMED-FALL  -> input LOW: a FALLING edge while still RISING-armed must capture NOTHING
#                  (the edge-select gate -- the negative control).
# Before the capture request line was wired, DMAEN[CA0DE] drove nothing and a capture-paced
# DMA moved nothing.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexpwm-capture.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal, sys

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-flexpwm-cap-%d.sock" % os.getpid()
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
        "property": "capture-a-input", "value": bool(level)}}) + "\n")
    qf.flush(); qf.readline()

out = []
deadline = time.time() + 12
os.set_blocking(p.stdout.fileno(), False)
while time.time() < deadline:
    line = p.stdout.readline()
    if line:
        s = line.decode(errors="replace")
        out.append(s)
        if "ARMED-RISE" in s:
            drive(True)         # rising edge -> capture
        elif "ARMED-CTRL" in s:
            drive(False)        # falling edge (input was HIGH) -> rising-armed must IGNORE
            drive(True)         # rising edge -> the ONE capture the control expects
        if "FLEXPWM-CAP PASS" in s or "FLEXPWM-CAP FAIL" in s:
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
echo "$OUT" | grep -q "FLEXPWM-CAP PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
