#!/usr/bin/env bash
# MCXN947 operator-fed PDM/MICFIL -> eDMA capture.
#
# The guest resets MICFIL, enables channel 0 with DISEL=DMA and watermark 0, arms an eDMA
# channel at DATACH0, and waits.  The harness pushes 8 known 24-bit PCM samples into channel
# 0 over QMP ("mic-input" QOM property) -- the board-farm audio-source path.  Each sample
# past the watermark asserts the MICFIL FIFO request (src 18, a level) and the eDMA drains it
# into memory.  The guest demands the exact sample sequence back, carried only by the DMA.
# Before the request line was wired, the empty FIFO never asserted and MICFIL DMA hung.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/pdm-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

OUT="$(QEMU="$QEMU" ELF="$ELF" python3 - <<'PY'
import socket, json, subprocess, time, os, signal, sys

# Same sequence the guest checks -- the OPERATOR is the data source.
MIC = [0x112233, 0x445566, 0x778899, 0xAABBCC,
       0x0D1E2F, 0x334455, 0x667788, 0x99AABB]

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-pdm-dma-%d.sock" % os.getpid()
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
        if "PDM-DMA ARMED" in s and not injected:
            for sample in MIC:            # operator streams the mic samples
                qf.write(json.dumps({"execute": "qom-set", "arguments": {
                    "path": "/machine/soc/pdm0",
                    "property": "mic-input", "value": sample}}) + "\n")
                qf.flush(); qf.readline()
            injected = True
        if "PDM-DMA PASS" in s or "PDM-DMA FAIL" in s:
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
echo "$OUT" | grep -q "PDM-DMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
