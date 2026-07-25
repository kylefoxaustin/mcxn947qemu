#!/usr/bin/env bash
# MCXN947 eFlexPWM output waveform + dangerous-zero reset values.
#
# The guest asserts the RM reset values (DTCNT0/1 = 0x07FF, DISMAP0 = 0xFFFF -- guest-readable),
# then walks PWM_A output scenarios holding each one; the HARNESS samples the read-only
# "pwm-a-output" QOM property for each and checks it matches the expected level:
#   bracket 1   INIT bracketed by [VAL2,VAL3)          -> HIGH
#   below 0     INIT below VAL2                          -> LOW  (compare depends on the counter)
#   pola 0      bracketed, OCTRL[POLA] inverts           -> LOW
#   outen 0     OUTEN cleared                            -> LOW  (pin not driven)
#   faulted 0   latched fault mapped to PWM_A (DISMAP)   -> LOW  (safety force-off)
#   unmapped 1  fault mapping bit cleared                -> HIGH (mapping is real)
# PWMOUT-FAULT-SET/CLR ask the harness to drive/clear the fault-input pin.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexpwm-output.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal, sys

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-flexpwm-out-%d.sock" % os.getpid()
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

def qmp(cmd, args):
    qf.write(json.dumps({"execute": cmd, "arguments": args}) + "\n"); qf.flush()
    return json.loads(qf.readline())

def drive_fault(level):
    qmp("qom-set", {"path": "/machine/soc/pwm0", "property": "fault-input", "value": bool(level)})

def read_output():
    r = qmp("qom-get", {"path": "/machine/soc/pwm0", "property": "pwm-a-output"})
    return 1 if r.get("return") else 0

results = []          # (tag, expected, actual)
resetvals = None
notes = []

deadline = time.time() + 40
os.set_blocking(p.stdout.fileno(), False)
while time.time() < deadline:
    line = p.stdout.readline()
    if not line:
        time.sleep(0.02); continue
    s = line.decode(errors="replace").strip()
    if not s:
        continue
    notes.append(s)
    tok = s.split()
    if s == "PWMOUT-FAULT-SET":
        drive_fault(1)
    elif s == "PWMOUT-FAULT-CLR":
        drive_fault(0)
    elif s == "PWMOUT-DONE":
        break
    elif tok[0] == "RESETVALS":
        resetvals = (s == "RESETVALS ok=1")
    elif tok[0] == "PWMOUT" and len(tok) == 3:
        exp = int(tok[2]); act = read_output()
        results.append((tok[1], exp, act))

p.send_signal(signal.SIGTERM)
try:
    p.wait(timeout=3)
except Exception:
    p.kill()
if os.path.exists(sock):
    os.remove(sock)

print("--- guest markers ---")
for n in notes:
    print(n)
print("--- output samples (tag expected actual) ---")
allok = (resetvals is True) and len(results) == 6
for tag, exp, act in results:
    ok = (exp == act)
    allok = allok and ok
    print("  %-10s exp=%d act=%d %s" % (tag, exp, act, "OK" if ok else "MISMATCH"))
print("resetvals=%s  checks=%d/6" % (resetvals, sum(1 for _,e,a in results if e==a)))
print("PWMOUT PASS" if allok else "PWMOUT FAIL")
PY
)"
echo "$OUT"
echo "$OUT" | grep -q "PWMOUT PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
