#!/usr/bin/env bash
# MCXN947 USBFS/KHCI HOST mass storage (MSC/BOT): the MCX enumerates an attached usb-storage
# device, READS block 0 via SCSI READ(10), and WRITES block 1 via SCSI WRITE(10) over Bulk-Only
# Transport -- verifying the ACTUAL backing-image contents (a host file the model cannot
# fabricate).  The write is ALSO re-checked on the host after QEMU exits: the bytes must have
# reached the real file, an oracle neither the guest nor the model can fake.
# PASS = guest "USB-MSC RW OK" AND the host sees the written signature in block 1.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/usb-host-msc.elf"
IMG="$HERE/usbdisk.img"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

# 64 KiB raw disk: block 0 carries a READ signature; block 1 starts blank (the guest writes it).
python3 - "$IMG" <<'PY'
import sys
p=sys.argv[1]; d=bytearray(64*1024)
d[0:8]=b"MCXN-USB"; d[508:512]=b"\xDE\xAD\xBE\xEF"      # block 0 (read oracle)
open(p,"wb").write(d)
PY

OUT="$(timeout -k 5 30 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -kernel "$ELF" -no-reboot \
        -drive if=none,id=d0,file="$IMG",format=raw \
        -device usb-storage,bus=usb-bus.0,port=1,drive=d0 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"

echo "$OUT" | grep -q "USB-MSC RW OK" || { echo "FAIL: guest did not report RW OK"; exit 1; }

# HOST-SIDE ORACLE: block 1 (offset 512) of the backing file must now hold the written signature.
python3 - "$IMG" <<'PY' || { echo "FAIL: written bytes did not reach the backing file"; exit 1; }
import sys
b=open(sys.argv[1],"rb").read()
blk1=b[512:1024]
assert blk1[0:16]==b"MCX-WROTE-THIS!!", ("block1 head=%r" % bytes(blk1[0:16]))
assert blk1[508:512]==b"\xCA\xFE\xF0\x0D", ("block1 tail=%r" % bytes(blk1[508:512]))
print("host oracle: block 1 written to the real backing file -- verified")
PY

echo "PASS"; exit 0
