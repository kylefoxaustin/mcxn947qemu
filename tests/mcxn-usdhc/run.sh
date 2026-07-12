#!/usr/bin/env bash
# MCXN947 uSDHC data path — real SD card, real ADMA2, golden on the host.
#
# The model supplies only the BUS.  We attach a genuine QEMU sd-card backed by
# an image file WE build here, so the golden lives somewhere the uSDHC model
# cannot reach.  Three passes:
#
#   1  CARD    — enumerate, read a block the HOST wrote, write a block via ADMA2
#                and read it back byte-exact.
#   2  HOST    — after QEMU exits, verify the written block really landed in the
#                image FILE.  A model that faked the round trip in RAM fails here.
#   3  NO CARD — with an empty slot, commands must TIME OUT (CTOE) and CINST must
#                read 0, instead of the host answering for a card that isn't there.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/usdhc.elf"
IMG="$HERE/card.img"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

# --- build the golden card image ------------------------------------------
# Block 0 = host_byte(i) = 0xC3 ^ (i*7 & 0xff).  The guest never writes this, so
# it cannot be produced by an echoing model.  QEMU's sd-card wants a
# power-of-two image size.
python3 - "$IMG" <<'PY'
import sys
size = 64 * 1024 * 1024
buf = bytearray(size)
for i in range(512):
    buf[i] = 0xC3 ^ ((i * 7) & 0xFF)
open(sys.argv[1], 'wb').write(buf)
PY

# --- pass 1: with a real card ----------------------------------------------
OUT="$(timeout 30 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot \
        -drive if=none,id=sd0,format=raw,file="$IMG" \
        -device sd-card,drive=sd0 2>/dev/null || true)"
echo "--- guest output (card) ---"; echo "$OUT"; echo "---------------------------"
if ! echo "$OUT" | grep -q "USDHC PASS"; then
    echo "FAIL: guest did not report a byte-exact block round trip"
    exit 1
fi

# --- pass 2: the write must have really left the emulator ------------------
python3 - "$IMG" <<'PY' || exit 1
import sys
want = bytes(((0xA5 + i * 31) & 0xFF) for i in range(512))
got = open(sys.argv[1], 'rb').read(1024)[512:1024]
if got != want:
    print("FAIL: block 1 in the image file does not match what the guest wrote")
    print("  first mismatch at byte",
          next(i for i in range(512) if got[i] != want[i]))
    sys.exit(1)
print("host-side check: block 1 in the image file is byte-exact")
PY

# --- pass 3: an empty slot must time out, not answer -----------------------
OUT2="$(timeout 30 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output (no card) ---"; echo "$OUT2"; echo "------------------------------"
if ! echo "$OUT2" | grep -q "USDHC NOCARD"; then
    echo "FAIL: with no card the host still claimed one / answered a command"
    exit 1
fi

rm -f "$IMG"
echo "PASS"
