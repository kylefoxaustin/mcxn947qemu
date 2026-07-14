#!/usr/bin/env bash
#
# THE REGISTERS NO AUTOMATED GATE CAN WATCH — and which therefore nobody was watching.
#
# tests/mcxn-reset-values is the strongest check in this tree: its golden is the RM, so
# it cannot be satisfied by the model agreeing with itself.  But it has TWO STRUCTURAL
# BLIND SPOTS, and both of them turned out to be hiding real bugs:
#
# 1. PER-INSTANCE RESET VALUES.  The golden stores ONE reset per register NAME and shares
#    it across instances.  When the RM gives a register two different values -- because
#    the value genuinely DIFFERS PER INSTANCE -- the extractor sees a contradiction and
#    REFUSES the row.  THAT IS CORRECT: a missing register is a gap, a wrong one is a
#    FALSE WITNESS, and a wrong golden makes the CHECKER lie.
#
#        ⭐ BUT A REFUSAL IS NOT A CHECK.  I grepped the golden for what the extractor
#           SKIPPED rather than what it checked, and the ENTIRE refusal pile was one
#           register: eDMA CH_SBR, all 16 channels, DMA0=6 / DMA1=7.
#
# 2. MULTIPLEXED WINDOWS.  FlexComm decodes as LPUART / LPSPI / LPI2C depending on
#    PSELID[PERSEL], which is 0 at reset -- so the gate, which probes AT RESET, reads the
#    LPUART map and CANNOT SEE the LPI2C registers at all.
#
# WHAT WAS HIDING IN THEM:
#
#   eDMA CH_SBR[MID] is the BUS MASTER ID (RM: DMA0=6, DMA1=7).  We reset it to ZERO.  And
#   Linux's fsl-edma READ-MODIFY-WRITES that register --
#       val = read(ch_sbr);  val |= EDMA_V3_CH_SBR_RD;  write(val);
#   -- so it READ OUR ZERO AND WROTE IT BACK AS ITS OWN CONFIGURATION, issuing every
#   transfer under the wrong master ID.  Nothing failed here, because nothing in THIS
#   model reads those bits.  IT FAILS ON HARDWARE.
#
#       ⭐ A REGISTER THE GUEST READ-MODIFY-WRITES IS THE ONE PLACE A ZERO RESET VALUE
#          SURVIVES INTO THE GUEST'S OWN STATE.  The RMW registers are where the claim
#          gets LAUNDERED.                                   (91emulator, same bug, same week)
#
#   LPI2C MRDROR is the NON-DESTRUCTIVE alias of MRDR -- and it was not modelled, so it
#   answered ZERO.  RXEMPTY is bit 14, so zero means THE RECEIVE FIFO HAS DATA.  A driver
#   polling the alias (which is what an alias is FOR) read a PHANTOM BYTE out of an empty
#   FIFO.  Same for the slave registers.  MRDR itself was correct; ITS ALIAS WAS NOT.
#
#       ⭐ AN UNMODELLED REGISTER IS NOT A FREE REGISTER.  IT STILL ANSWERS -- AND ZERO
#          IS AN ANSWER.
#
# Every number below is HAND-READ FROM THE RM and appears nowhere in the model.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

DMA0=0x40080000; DMA1=0x400A0000; FC0=0x40092000
QS="$(mktemp)"; trap 'rm -f "$QS"' EXIT
{
  # -- eDMA CH_SBR[MID]: per-instance, and READ-MODIFY-WRITTEN by the guest.
  printf 'readl 0x%x\n' $((DMA0 + 0x100C))            # DMA0 CH0_SBR  -> 6
  printf 'readl 0x%x\n' $((DMA0 + 0x100C + 15*0x1000))   # DMA0 CH15_SBR -> 6
  printf 'readl 0x%x\n' $((DMA1 + 0x100C))            # DMA1 CH0_SBR  -> 7
  # -- LPI2C: the gate cannot see these (PERSEL=0 at reset decodes as LPUART).
  printf 'writel 0x%x 0x3\n' $((FC0 + 0xFF8))         # PSELID[PERSEL] = LPI2C
  printf 'readl 0x%x\n' $((FC0 + 0x870))              # MRDR    -> RXEMPTY 0x4000
  printf 'readl 0x%x\n' $((FC0 + 0x878))              # MRDROR  -> RXEMPTY 0x4000  (the alias)
  printf 'readl 0x%x\n' $((FC0 + 0x950))              # SASR    -> 0x4000
  printf 'readl 0x%x\n' $((FC0 + 0x970))              # SRDR    -> 0x4000
  printf 'readl 0x%x\n' $((FC0 + 0x978))              # SRDROR  -> 0x4000
} > "$QS"

NAMES=("DMA0.CH0_SBR" "DMA0.CH15_SBR" "DMA1.CH0_SBR"
       "LPI2C0.MRDR" "LPI2C0.MRDROR" "LPI2C0.SASR" "LPI2C0.SRDR" "LPI2C0.SRDROR")
WANT=(6 6 7 16384 16384 16384 16384 16384)

mapfile -t V < <(timeout -k 5 60 "$QEMU" -M frdm-mcxn947 -display none -accel qtest \
                     -qtest stdio -monitor none -serial none < "$QS" 2>/dev/null \
                 | grep -oE '^OK 0x[0-9a-f]+' | cut -d' ' -f2)

[ "${#V[@]}" -eq "${#NAMES[@]}" ] || {
    echo "FAIL: asked ${#NAMES[@]} questions, got ${#V[@]} answers"; exit 1; }

fail=0
for i in "${!NAMES[@]}"; do
    got=$(printf '%d' "${V[$i]}")
    if [ "$got" -eq "${WANT[$i]}" ]; then
        printf '  PASS  %-14s = 0x%08x\n' "${NAMES[$i]}" "$got"
    else
        printf '  FAIL  %-14s = 0x%08x, RM says 0x%08x\n' "${NAMES[$i]}" "$got" "${WANT[$i]}"
        fail=1
    fi
done
[ $fail -eq 0 ] && echo "PASS: the gate's blind spots hold — per-instance resets and the LPI2C aliases"
exit $fail
