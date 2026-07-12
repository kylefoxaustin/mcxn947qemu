#!/usr/bin/env bash
#
# The LPADC result FIFO: DEPTH, FCOUNT, the FWMARK watermark, and the FOFn
# overflow flag.  qtest, so it is exact and deterministic.
#
# ⭐ WHY THIS TEST EXISTS.  The model's "result FIFO" was
#
#       uint32_t fifo_data;   bool fifo_valid;        /* ONE SLOT */
#
# and a conversion arriving before the previous one was drained OVERWROTE it, with
# no flag and no error.  A conversion simply ceased to exist.
#
# The tell was not a failing test -- it was a FLAKY one.  tests/mcxn-adc-dma passed
# about one run in three, and I had "fixed" it once by putting a DELAY LOOP in the
# firmware so the eDMA could drain the single slot between triggers.  That crutch
# WAS the camouflage: with it, the depth-1 FIFO looked like a working one.  Running
# it under -icount (a deterministic instrument) turned the coin flip into a
# reproducible FAIL, and then the missing samples were obvious.
#
#     ⭐ A FLAKY TEST IS NOT A TEST THAT SOMETIMES FAILS.  IT IS A BUG YOU HAVE
#        AGREED TO SEE ONLY SOMETIMES.  Every green it ever gave me was luck, and
#        I had already spent that luck twice: once accepting the green, and once
#        adding the delay that manufactured it.
#
# And the depth was not merely "less buffering".  A depth-1 FIFO SILENTLY DISABLES
# THE FEATURE BUILT ON TOP OF IT: FCTRL[FWMARK] is a watermark, occupancy could
# only ever be 0 or 1, so ANY firmware asking to be woken at FWMARK >= 1 -- "tell
# me when 8 samples are ready", the entire purpose of the register -- waited
# forever.  The model had every bit of the watermark plumbing and nothing to
# watermark.
#
# Facts here are from the RM (ADC chapter) and the CMSIS header, not from me:
#   - "Supports 16-word depth FIFO with the configurable watermark."
#   - STAT[FOF0] bit 1: "more data has been written to the Result FIFO than it can
#     hold.  THE NEWER DATA IS NOT STORED and the FIFO holds the original contents."
#   - FCTRL[FCOUNT] bits 4:0, FCTRL[FWMARK] bits 19:16, STAT[RDY0] bit 0.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

ADC0=0x4010D000
r() { printf '0x%x' $((ADC0 + $1)); }
CTRL=$(r 0x010); STAT=$(r 0x014); SWTRIG=$(r 0x034); TCTRL0=$(r 0x0A0)
FCTRL0=$(r 0x0E0); CMDL1=$(r 0x100); RESFIFO0=$(r 0x300)

# Build one qtest script; QEMU echoes "OK 0x..." per readl, in order.
{
  echo "writel $CTRL 0x1"          # CTRL[ADCEN]
  echo "writel $CMDL1 0x3"         # CMD1: CMDL[ADCH] = channel 3
  echo "writel $TCTRL0 0x01000000" # TCTRL0[TCMD] = 1 -> CMD1; FIFO_SEL_A = 0
  echo "writel $FCTRL0 0x00030000" # FWMARK = 3
  echo "readl $STAT"               # [1] RDY0 must be CLEAR (0 entries <= wm 3)
  for _ in $(seq 1 3); do echo "writel $SWTRIG 0x1"; done
  echo "readl $FCTRL0"             # [2] FCOUNT == 3
  echo "readl $STAT"               # [3] RDY0 STILL CLEAR: 3 is NOT > 3
  echo "writel $SWTRIG 0x1"        # 4th
  echo "readl $STAT"               # [4] RDY0 SET now: 4 > 3
  for _ in $(seq 5 16); do echo "writel $SWTRIG 0x1"; done
  echo "readl $FCTRL0"             # [5] FCOUNT == 16 (full)
  echo "readl $STAT"               # [6] FOF0 still CLEAR (never overflowed)
  echo "writel $SWTRIG 0x1"        # 17th -- OVERFLOW
  echo "readl $STAT"               # [7] FOF0 SET
  echo "readl $FCTRL0"             # [8] FCOUNT still 16 (new data dropped)
  echo "readl $RESFIFO0"           # [9] head is the FIRST result, not the last
} > /tmp/adc-fifo-qt.$$

# -accel qtest: THE GUEST CPU MUST NOT BE RUNNING.
#
# Without it, `-qtest stdio` still starts a TCG vCPU, which boots from a ZEROED
# vector table (PC=0, SP=0), faults immediately, and eventually dies with
#   qemu: fatal: Lockup: can't escalate 3 to HardFault
# -- an ABORT, mid-test.  My ADC FIFO test still printed PASS through that abort,
# because it had already scraped its readl values off the wire.
#
#     ⭐ A CRASH AND A PASS MUST NEVER BE INDISTINGUISHABLE.  I wrote exactly that
#        gate into tests/mutate.sh and then failed to apply it to the next test I
#        wrote.  ("I patched it where I MET it.")
#
# And the abort was the LOUD half of the problem.  The QUIET half: a free-running
# CPU executing garbage is a SECOND WRITER to the address space I am reading.  It
# never corrupted a result here -- but nothing was stopping it, and "it happened
# not to" is not a property of a test, it is a property of that afternoon.
#
# THERE IS NO `quit` IN THE QTEST PROTOCOL.  There never was -- it answers
#   FAIL Unknown command 'quit'
# and keeps running.  So how did these tests ever terminate?  THE CRASH ABOVE
# TERMINATED THEM.  The rogue CPU locked up, QEMU aborted, the pipe closed, and
# the harness collected its results and called them a PASS.
#
#     ⭐ THE BUG WAS LOAD-BEARING.  Fixing the crash is what made the hang appear:
#        I had two defects whose only symptom was each other, and a green test.
#
# The harness ends the process itself (`timeout`), which is what QEMU's own qtest
# framework does, and then CHECKS IT GOT EVERY ANSWER IT ASKED FOR -- because a
# truncated conversation and a correct one differ only in the answers you never
# notice are missing.
OUT="$(timeout 30 "$QEMU" -M frdm-mcxn947 -display none -accel qtest -qtest stdio \
         -monitor none -serial none < /tmp/adc-fifo-qt.$$ 2>/dev/null || true)"
rm -f /tmp/adc-fifo-qt.$$
mapfile -t V < <(echo "$OUT" | grep -oE '^OK 0x[0-9a-f]+' | cut -d' ' -f2)

if echo "$OUT" | grep -q '^FAIL'; then
    echo "FAIL: the qtest protocol rejected a command:"
    echo "$OUT" | grep '^FAIL' | head -2 | sed 's/^/      /'
    exit 1
fi
[ "${#V[@]}" -eq 9 ] || { echo "FAIL: asked 9 questions, got ${#V[@]} answers"; exit 1; }
d() { printf '%d' "$1"; }
fail=0
chk() { # name  actual-expr  expected-expr  explain
    if [ "$2" = "$3" ]; then printf '  PASS  %s\n' "$1"
    else printf '  FAIL  %s (got %s, want %s)\n        %s\n' "$1" "$2" "$3" "$4"; fail=1; fi
}
FCOUNT() { printf '%d' $(( $(d "$1") & 0x1F )); }
RDY0()   { printf '%d' $(( ($(d "$1") >> 0) & 1 )); }
FOF0()   { printf '%d' $(( ($(d "$1") >> 1) & 1 )); }

chk "RDY0 clear when empty (0 <= FWMARK 3)"      "$(RDY0   "${V[0]}")" 0 "RDY is a level: occupancy > watermark"
chk "FCOUNT == 3 after 3 conversions"            "$(FCOUNT "${V[1]}")" 3 "a depth-1 FIFO reports at most 1 here"
chk "RDY0 STILL clear at FCOUNT==FWMARK (3)"     "$(RDY0   "${V[2]}")" 0 "RM: ready means STRICTLY greater than the watermark"
chk "RDY0 SET at FCOUNT 4 > FWMARK 3"            "$(RDY0   "${V[3]}")" 1 "the watermark is unusable on a 1-deep FIFO -- this is the bug"
chk "FCOUNT == 16 when full"                     "$(FCOUNT "${V[4]}")" 16 "RM: 16-word depth FIFO"
chk "FOF0 clear -- 16 entries is NOT overflow"   "$(FOF0   "${V[5]}")" 0 "off-by-one: the 16th must fit"
chk "FOF0 SET on the 17th conversion"            "$(FOF0   "${V[6]}")" 1 "silicon flags overflow; the old model dropped data SILENTLY"
chk "FCOUNT still 16 after overflow"             "$(FCOUNT "${V[7]}")" 16 "RM: the NEWER data is not stored"
chk "head is the FIRST result (FIFO, not LIFO)"  "$(( $(d "${V[8]}") & 0xFFFF ))" 2048 \
    "RM: 'the FIFO holds the original contents' -- overflow must not clobber the head"

[ $fail -eq 0 ] && echo "PASS: LPADC result FIFO — depth 16, FCOUNT, FWMARK watermark, FOF overflow"
exit $fail
