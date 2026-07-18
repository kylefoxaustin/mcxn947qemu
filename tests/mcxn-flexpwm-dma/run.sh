#!/usr/bin/env bash
# MCXN947 eFlexPWM value-register DMA: duty-cycle words streamed into VAL3 by the
# eDMA on each RELOAD, with the CPU never writing VAL3 after arming the channel.
#
# Two axes, against an oracle the FlexPWM model does not own:
#   DATA -- VAL3 must end holding the last duty word (carried purely by reload DMA);
#   RATE -- one word per reload, so under -icount the 8-word transfer takes ~8 carrier
#           periods measured against SysTick (an independent Arm core timer).
# Before the FlexPWM0 value request line (mux src 43) was wired, DMAEN[VALDE] drove
# nothing and a value-DMA control loop hung waiting for a request that could not assert.
#
# ⚠ -icount shift=3 is REQUIRED: the RATE axis measures elapsed VIRTUAL time, which must
# track the carrier and not the noisy host clock.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/flexpwm-dma.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
OUT="$(timeout -k 5 20 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -icount shift=3 -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "FLEXPWM-DMA PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
