#!/usr/bin/env bash
# MCXN947 CMP-crossing-paced eDMA (operator-driven).
#
# The guest arms an eDMA channel at the CMP DMA request (mux src 28), sets CCR1[DMA_EN] +
# IER[CFR_IE], and waits.  The harness injects the comparator output HIGH over QMP
# ("comparator-output" QOM property) -- the board-farm operator path for an analog event.
# The rising edge, redirected by DMA_EN, pulses the eDMA request and moves EXACTLY ONE word.
# Before the CMP DMA line was wired, DMA_EN drove nothing and the transfer hung.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/cmp-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal, sys

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-cmp-dma-%d.sock" % os.getpid()
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
        if "CMP-DMA ARMED" in s and not injected:
            # Operator drives the comparator output high: ONE rising edge.
            qf.write(json.dumps({"execute": "qom-set", "arguments": {
                "path": "/machine/soc/cmp0",
                "property": "comparator-output", "value": True}}) + "\n")
            qf.flush(); qf.readline()
            injected = True
        if "CMP-DMA PASS" in s or "CMP-DMA FAIL" in s:
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
echo "$OUT" | grep -q "CMP-DMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
