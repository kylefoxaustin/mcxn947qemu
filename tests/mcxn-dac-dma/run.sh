#!/usr/bin/env bash
# MCXN947 peripheral-triggered eDMA — DAC waveform streamed entirely by the DMA.
#
# The CPU never writes DAC_DATA.  Every sample reaching the converter is carried
# by the eDMA, one minor loop per DAC FIFO-watermark request (mux source 25 —
# a DIFFERENT source from the SAI test, so this also proves the request-MUX
# routing is not SAI-specific).
#
# A DAC's answer only exists ON THE PIN — the guest cannot read back what it
# converted.  So the OPERATOR probes it over QMP (qom-get "analog-output"),
# exactly as a bench engineer scopes the output.  The golden is the sample buffer
# in the firmware; the observation point is outside the model's register file.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
export ELF="$HERE/dac-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

python3 - <<'PY'
import socket, json, subprocess, time, os, sys

qemu = os.environ["QEMU"]; elf = os.environ["ELF"]
sock = "/tmp/mcxn-dac-dma-%d.sock" % os.getpid()
if os.path.exists(sock):
    os.remove(sock)

# The waveform the firmware streams (tests/mcxn-dac-dma/main.c).
WANT = [0x0ABC, 0x0123, 0x0FFF, 0x0555, 0x0AAA, 0x0001]

p = subprocess.Popen(
    [qemu, "-M", "frdm-mcxn947", "-display", "none", "-monitor", "none",
     "-serial", "stdio", "-kernel", elf, "-no-reboot",
     "-qmp", "unix:%s,server,nowait" % sock],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

for _ in range(200):
    if os.path.exists(sock):
        break
    time.sleep(0.05)
qs = socket.socket(socket.AF_UNIX); qs.connect(sock); qf = qs.makefile('rw')
qf.readline()
qf.write(json.dumps({"execute": "qmp_capabilities"}) + "\n"); qf.flush()
qf.readline()

def probe_pin():
    """Read the DAC's analog output — the operator's scope on the pin."""
    qf.write(json.dumps({"execute": "qom-get", "arguments": {
        "path": "/machine/soc/dac0", "property": "analog-output"}}) + "\n")
    qf.flush()
    return json.loads(qf.readline())["return"]

got, guest_ok, out = [], False, []
deadline = time.time() + 20
while time.time() < deadline:
    line = p.stdout.readline()
    if not line:
        break
    s = line.decode(errors="replace")
    out.append(s)
    if "TRIG" in s:
        got.append(probe_pin())
        # Release the guest: it is holding the pin steady for us.
        p.stdin.write(b"x"); p.stdin.flush()
    if "DACDMA GUEST-OK" in s:
        guest_ok = True
        break
    if "DACDMA FAIL" in s:
        break

p.kill()
try:
    os.remove(sock)
except OSError:
    pass

print("--- guest output ---")
print("".join(out).strip())
print("--------------------")
print("pin samples read by the operator:", [hex(v) for v in got])
print("golden (firmware's buffer):      ", [hex(v) for v in WANT])

if not guest_ok:
    print("FAIL: guest reported an underflow / the DMA did not keep it fed")
    sys.exit(1)
if got != WANT:
    print("FAIL: the converted waveform is not the one the DMA was told to stream")
    sys.exit(1)
print("PASS")
PY
