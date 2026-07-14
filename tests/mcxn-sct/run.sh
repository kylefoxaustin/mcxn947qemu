#!/usr/bin/env bash
# MCXN947 SCT: the counter PERIOD measured against SysTick, with the PRESCALER
# SWEPT.  See main.c -- the old version counted interrupts, which is blind to a
# fabricated tick rate AND to a prescaler that is not modelled at all.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/sct.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"
# -icount shift=3: this is now a TIMING test, and a timing measurement on a
# NON-DETERMINISTIC instrument tracks HOST time.  A golden compared against a noisy
# measurement gives a confident, reproducible-looking, WRONG answer.  (Without it
# this test reads 5301 ticks where the RM says 2052, and the number changes per run.)
OUT="$(timeout -k 5 120 "$QEMU" -M frdm-mcxn947 -display none -monitor none -icount shift=3 \
        -serial stdio -kernel "$ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
echo "$OUT" | grep -q "SCT PASS" && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
