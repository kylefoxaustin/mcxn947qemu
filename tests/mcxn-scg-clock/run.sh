#!/usr/bin/env bash
#
# THE RESET VALUE IS PART OF THE MODEL.  A register you never wrote is still a
# register the guest READS -- and a WRONG value there is a silent wrong answer with
# no code path to blame.
#
# THE BUG THIS PINS.  mcxn_scg_reset() did `memset(s->regs, 0, sizeof(s->regs))`,
# which felt like a safe, neutral default.  It is not neutral.  The RM (rev 7, SCG
# register map) gives SIRCCSR a reset value of 0100_0020h, and bit 5 of that is
# SIRC_CLK_PERIPH_EN -- SET OUT OF RESET.  The SDK reads it:
#
#     static uint32_t CLOCK_GetFro12MFreq(void) {
#         return ((SCG0->SIRCCSR & SCG_SIRCCSR_SIRC_CLK_PERIPH_EN_MASK) != 0UL)
#                ? 12000000U : 0U;                        /* fsl_clock.c:2248 */
#     }
#
# With the bit clear that is 0 Hz, so CLOCK_GetLPFlexCommClkFreq() is 0 Hz, and
# EVERY FlexComm driver init -- LPI2C, LPSPI, LPUART -- trips
# assert(sourceClock_Hz > 0U) and HARD-FAULTS.  One bit in one reset value took out
# an entire peripheral family, and nothing in the guest's clock_config.c ever sets
# it BECAUSE ON SILICON IT IS ALREADY SET.
#
#     ⭐ A ZERO RESET VALUE IS NOT THE ABSENCE OF A CLAIM.  IT IS A CLAIM THAT THE
#        BIT IS ZERO, AND THE GUEST WILL BELIEVE IT.
#
# TWO ORACLES, BECAUSE ONE OF THEM IS MINE AND I DO NOT TRUST MY OWN.
#
#   A. qtest: SIRCCSR == 0100_0020h.  The golden is the RM, not my header.  But I
#      am the one who typed it into both places, so on its own this is close to
#      asking the suspect whether he did it.
#
#   B. THE REAL TENANT: NXP's own unmodified lpi2c_interrupt example.  It calls
#      LPI2C_MasterInit(), which queries the clock and asserts if it is 0.  If the
#      reset value regresses, THE VENDOR'S DRIVER SAYS SO, in its own words, on the
#      console -- an oracle I did not write and cannot accidentally agree with.
#
# What B does NOT claim: that the example COMPLETES.  It is an on-board
# master<->slave example that needs a physical jumper between two FlexComm
# instances' pins, and I do not model board-level pin wiring.  It gets through init
# and stops.  Saying more than that would be the green light with nothing behind it.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
WS="${MCUX_WS:-$HOME/mcux-ws}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

SCG0=0x40044000
SIRCCSR=$((SCG0 + 0x200))
EXPECT=0x01000020        # RM rev 7: "200h  SIRCCSR  32  RW  0100_0020h"
fail=0

# ── ORACLE A — the reset value itself, against the RM.
# -accel qtest so the guest CPU is HALTED.  Without it, `-qtest stdio` still runs a
# TCG vCPU from a zeroed vector table; it faults, locks up, and QEMU aborts -- and
# that abort was the only thing ENDING this test (there is no `quit` command in the
# qtest protocol; it answers FAIL Unknown command).  A crashing QEMU was my exit
# path, and the harness read its answers off the wire and called it a PASS.
QOUT="$(printf 'readl %s\n' "$(printf '0x%x' $SIRCCSR)" \
        | timeout 30 "$QEMU" -M frdm-mcxn947 -display none -accel qtest -qtest stdio \
              -monitor none -serial none 2>/dev/null || true)"
got=$(echo "$QOUT" | grep -oE '^OK 0x[0-9a-f]+' | cut -d' ' -f2 | tail -1)
if [ "$(printf '%d' "$got" 2>/dev/null || echo -1)" -eq "$(printf '%d' $EXPECT)" ]; then
    echo "PASS: SIRCCSR reset = $got (RM: 0100_0020h, SIRC_CLK_PERIPH_EN set)"
else
    echo "FAIL: SIRCCSR reset = ${got:-<none>}, RM says $EXPECT"
    echo "      bit 5 (SIRC_CLK_PERIPH_EN) drives CLOCK_GetFro12MFreq(); if it is"
    echo "      clear the SDK sees a 0 Hz FlexComm clock and every LPI2C/LPSPI/LPUART"
    echo "      init asserts and hard-faults."
    fail=1
fi

# ── ORACLE B — the real tenant.  NXP's driver, NXP's assert, NXP's words.
ELF="$WS/examples/frdmmcxn947/driver_examples/lpi2c/interrupt/cm33_core0/armgcc/debug/lpi2c_interrupt.elf"
if [ ! -f "$ELF" ]; then
    echo "SKIP: stock lpi2c_interrupt example not staged (set MCUX_WS)"
    exit $fail
fi

timeout 15 "$QEMU" -M frdm-mcxn947 -display none -monitor none -no-reboot \
    -serial "file:$TMP/out.txt" -kernel "$ELF" >/dev/null 2>&1 || true
OUT="$(tr -d '\r' < "$TMP/out.txt" 2>/dev/null)"

if echo "$OUT" | grep -qiE 'ASSERT ERROR|HardFault|sourceClock_Hz'; then
    echo "FAIL: the stock NXP LPI2C driver rejected the model:"
    echo "$OUT" | grep -iE 'ASSERT ERROR|HardFault|sourceClock_Hz' | head -2 | sed 's/^/      /'
    fail=1
elif echo "$OUT" | grep -q 'LPI2C example'; then
    echo "PASS: stock lpi2c_interrupt reaches LPI2C_MasterInit and past it (no assert)"
else
    echo "FAIL: stock lpi2c_interrupt produced no banner at all — it died before init."
    echo "$OUT" | tail -3 | sed 's/^/      /'
    fail=1
fi

exit $fail
