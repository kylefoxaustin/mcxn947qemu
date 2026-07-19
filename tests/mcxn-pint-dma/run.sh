#!/usr/bin/env bash
# MCXN947 PINT-edge-paced eDMA + NVIC delivery (operator-driven).
#
# The guest arms an eDMA channel at the PINT INT0 request (mux src 3), enables a rising-edge
# on channel 0, and waits.  The harness injects the pin level HIGH over QMP ("pin-input" QOM
# property) -- the board-farm operator path for a pin edge.  One rising edge moves EXACTLY
# one word (DMA), latches RISE/IST (edge-detect), and -- once IRQ 47 is enabled -- fires the
# shared PINT handler (NVIC).  Before PINT was functional, none of this happened.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/pint-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal, sys

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-pint-dma-%d.sock" % os.getpid()
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

out = []
injected = False
deadline = time.time() + 10
os.set_blocking(p.stdout.fileno(), False)
while time.time() < deadline:
    line = p.stdout.readline()
    if line:
        s = line.decode(errors="replace")
        out.append(s)
        if "PINT-DMA ARMED" in s and not injected:
            # Operator drives channel-0 input HIGH: one rising edge.
            qf.write(json.dumps({"execute": "qom-set", "arguments": {
                "path": "/machine/soc/pint0",
                "property": "pin-input", "value": 1}}) + "\n")
            qf.flush(); qf.readline()
            injected = True
        if "PINT-DMA PASS" in s or "PINT-DMA FAIL" in s:
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
echo "$OUT" | grep -q "PINT-DMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
