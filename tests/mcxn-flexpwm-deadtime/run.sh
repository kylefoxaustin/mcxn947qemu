#!/usr/bin/env bash
# MCXN947 eFlexPWM complementary pair + dead-time insertion.
# The guest holds the static counter at chosen positions; the harness reads pwm-a-output AND
# pwm-b-output for each and checks against the expected (A,B).  The dead band shows as BOTH
# reading 0; the withdead/nodead pair (same INIT=VAL2, dead-time toggled) proves the insertion.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexpwm-deadtime.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal, sys

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-flexpwm-dt-%d.sock" % os.getpid()
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
qf.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); qf.flush(); qf.readline()

def rd(prop):
    qf.write(json.dumps({"execute": "qom-get", "arguments": {
        "path": "/machine/soc/pwm0", "property": prop}}) + "\n"); qf.flush()
    return 1 if json.loads(qf.readline()).get("return") else 0

results = []
deadline = time.time() + 40
os.set_blocking(p.stdout.fileno(), False)
while time.time() < deadline:
    line = p.stdout.readline()
    if not line:
        time.sleep(0.02); continue
    s = line.decode(errors="replace").strip()
    if not s:
        continue
    print(s)
    tok = s.split()
    if s == "DT-DONE":
        break
    if tok[0] == "DT" and len(tok) == 4:
        tag, ea, eb = tok[1], int(tok[2]), int(tok[3])
        aa, ab = rd("pwm-a-output"), rd("pwm-b-output")
        results.append((tag, ea, eb, aa, ab))

p.send_signal(signal.SIGTERM)
try:
    p.wait(timeout=3)
except Exception:
    p.kill()
if os.path.exists(sock):
    os.remove(sock)

print("--- samples (tag expA expB gotA gotB) ---")
allok = len(results) == 6
for tag, ea, eb, aa, ab in results:
    ok = (ea == aa and eb == ab)
    allok = allok and ok
    print("  %-10s exp=(%d,%d) got=(%d,%d) %s" % (tag, ea, eb, aa, ab, "OK" if ok else "MISMATCH"))
print("DEADTIME PASS" if allok else "DEADTIME FAIL")
PY
)"
echo "$OUT"
echo "$OUT" | grep -q "DEADTIME PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
