#!/usr/bin/env bash
# MCXN947 eFlexPWM PWM_X auxiliary output.  Guest holds the static counter at chosen positions;
# the harness reads pwm-x-output for each and checks it, plus a fault force-off scenario.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexpwm-pwmx.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal
qemu=os.environ["QEMU"]; elf=os.environ["ELF"]
sock="/tmp/mcxn-flexpwm-pwmx-%d.sock"%os.getpid()
if os.path.exists(sock): os.remove(sock)
p=subprocess.Popen([qemu,"-M","frdm-mcxn947","-display","none","-monitor","none",
  "-serial","stdio","-kernel",elf,"-no-reboot","-qmp","unix:%s,server,nowait"%sock],
  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
for _ in range(100):
    if os.path.exists(sock): break
    time.sleep(0.05)
qs=socket.socket(socket.AF_UNIX); qs.connect(sock); qf=qs.makefile('rw')
qf.readline(); qf.write(json.dumps({"execute":"qmp_capabilities"})+"\n"); qf.flush(); qf.readline()
def drive(v):
    qf.write(json.dumps({"execute":"qom-set","arguments":{"path":"/machine/soc/pwm0","property":"fault-input","value":bool(v)}})+"\n"); qf.flush(); qf.readline()
def rdx():
    qf.write(json.dumps({"execute":"qom-get","arguments":{"path":"/machine/soc/pwm0","property":"pwm-x-output"}})+"\n"); qf.flush()
    return 1 if json.loads(qf.readline()).get("return") else 0
results=[]; deadline=time.time()+30
os.set_blocking(p.stdout.fileno(), False)
while time.time()<deadline:
    line=p.stdout.readline()
    if not line: time.sleep(0.02); continue
    s=line.decode(errors="replace").strip()
    if not s: continue
    print(s); tok=s.split()
    if s=="PWMX-FAULT-SET": drive(1)
    elif s=="PWMX-FAULT-CLR": drive(0)
    elif s=="PWMX-DONE": break
    elif tok[0]=="PWMX" and len(tok)==3:
        results.append((tok[1], int(tok[2]), rdx()))
p.send_signal(signal.SIGTERM)
try: p.wait(timeout=3)
except Exception: p.kill()
if os.path.exists(sock): os.remove(sock)
print("--- samples (tag exp got) ---")
allok=len(results)==5
for tag,e,g in results:
    ok=(e==g); allok=allok and ok
    print("  %-8s exp=%d got=%d %s"%(tag,e,g,"OK" if ok else "MISMATCH"))
print("PWMX PASS" if allok else "PWMX FAIL")
PY
)"
echo "$OUT"
echo "$OUT" | grep -q "PWMX PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
