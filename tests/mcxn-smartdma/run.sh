#!/usr/bin/env bash
# MCXN947 SMARTDMA register-fidelity test.  The EZH datapath is NOT modelled
# (proprietary microcode, no ISA in the RM), so this proves the model is HONEST
# and INFORMATIVE, never silently wrong:
#   1. HONEST-FAULT (serial): a keyed BOOT leaves CTRL.START set — the engine
#      never completes; it does not self-clear over an untouched buffer.
#   2. INFORMATIVE (log): the model recovers the requested apiIndex from the
#      firmware jump table and names the op (apiIndex 4 = RGB565To888).
#   3. KEYED CTRL (log): a keyless START-looking write is rejected, not booted
#      (exactly ONE boot in the log).
# The QEMU log is captured via -D (a file QEMU flushes), not stderr, so it
# survives the timeout SIGTERM.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/smartdma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

SERIAL="$(mktemp)"; LOG="$(mktemp)"
trap 'rm -f "$SERIAL" "$LOG"' EXIT
timeout -k 5 12 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
    -serial "file:$SERIAL" -D "$LOG" -d unimp,guest_errors \
    -kernel "$ELF" -no-reboot >/dev/null 2>&1 || true

OUT="$(cat "$SERIAL" 2>/dev/null)"
LOGC="$(cat "$LOG" 2>/dev/null)"
BOOTS="$(printf '%s\n' "$LOGC" | grep -c 'SmartDMA BOOT requested' || true)"
echo "--- guest console ---"; echo "$OUT"
echo "--- qemu log (smartdma) ---"; printf '%s\n' "$LOGC" | grep -i smartdma
echo "--- boots counted: $BOOTS ---"

# 1. honest-fault, guest-visible: START stayed set after boot, no false completion
c1=$(echo "$OUT" | grep -qc "START-STILL-SET" && echo 1 || echo 0)
c1b=$(echo "$OUT" | grep -qc "INIT-NO-START"   && echo 1 || echo 0)
c1c=$(echo "$OUT" | grep -qc "SMARTDMA OK"      && echo 1 || echo 0)
# 2. informative: apiIndex recovered + op named
c2=$(printf '%s' "$LOGC" | grep -qc "op=RGB565To888" && echo 1 || echo 0)
c2b=$(printf '%s' "$LOGC" | grep -qc "apiIndex=4"    && echo 1 || echo 0)
# 3. keyed CTRL: keyless write rejected AND not booted (exactly one boot)
c3=$(printf '%s' "$LOGC" | grep -qc "lacks the 0xC0DE key" && echo 1 || echo 0)

if [ "$c1" = 1 ] && [ "$c1b" = 1 ] && [ "$c1c" = 1 ] \
   && [ "$c2" = 1 ] && [ "$c2b" = 1 ] \
   && [ "$c3" = 1 ] && [ "$BOOTS" = 1 ]; then
    echo "PASS: honest-fault (START stays set) + apiIndex recovered (RGB565To888) + keyed CTRL enforced"
    exit 0
else
    echo "FAIL (START-set=$c1 init-no-start=$c1b ok=$c1c op-named=$c2 apiIndex=$c2b keyless-rejected=$c3 boots=$BOOTS)"
    exit 1
fi
