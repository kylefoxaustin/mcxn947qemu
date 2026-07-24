#!/usr/bin/env bash
# MCXN947 QSPI execute-in-place BOOT: the M33 resets and runs entirely from the external
# FlexSPI NOR (secure XIP alias 0x90000000), no internal-flash image.  `-machine qspi-boot=on`
# resets from the XIP window; the image carries a FlexSPI Config Block (tag "FCFB") at
# 0x90000400, which the boot ROM validates.  Two checks:
#   POSITIVE  -- the QSPI image boots and runs in place (prints QSPI-BOOT PASS).
#   NEGATIVE  -- the SAME machine option with an FCB-less image (any internal-flash ELF) is
#                REJECTED by the boot-ROM FCB check (QEMU exits non-zero with a loud message).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/qspi-boot.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

# --- POSITIVE: boot from QSPI ------------------------------------------------
OUT="$(timeout -k 5 15 "$QEMU" -M frdm-mcxn947,qspi-boot=on -display none -monitor none \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "QSPI-BOOT PASS" || { echo "FAIL: QSPI image did not boot in place"; exit 1; }

# --- NEGATIVE: an FCB-less image must be rejected by the boot-ROM check -------
# The internal-flash gpio ELF is linked at 0x10000000, so its 0x90000400 is empty -> no FCB.
NOFCB="$HERE/../mcxn-gpio/gpio.elf"
if [ -f "$NOFCB" ]; then
    ERR="$("$QEMU" -M frdm-mcxn947,qspi-boot=on -display none -monitor none -serial null \
            -kernel "$NOFCB" -no-reboot 2>&1 || true)"
    echo "$ERR" | grep -q "no valid FlexSPI Config Block" || {
        echo "FAIL: FCB-less QSPI image was NOT rejected (boot-ROM check is dead)"; exit 1; }
    echo "negative control: FCB-less image correctly rejected"
fi

echo "PASS"; exit 0
