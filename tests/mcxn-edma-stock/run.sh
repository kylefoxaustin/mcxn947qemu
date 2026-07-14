#!/usr/bin/env bash
#
# THE REAL TENANT, for the eDMA ENGINE ITSELF.  Runs NXP's own unmodified SDK
# edma3 driver examples and checks each one's DATA, not just that it started.
#
# My hand-written eDMA tests all passed.  These found real bugs none of them could:
#
#   scatter_gather -> "1 2 3 4 0 0 0 0"  TCD chaining (CSR[ESG] + DLAST_SGA) was not
#                                        modelled: only the FIRST TCD ran and half
#                                        the transfer went SILENTLY MISSING.
#   wrap_transfer  -> "1 2 3 4 150000000 16000000 32768 0"
#                                        ☠ THE WORST.  ATTR[SMOD]/[DMOD] -- the
#                                        circular-buffer modulo -- was not modelled,
#                                        so the engine ran off the end of its source
#                                        buffer and handed the guest ADJACENT MEMORY.
#                                        Not zeros.  PLAUSIBLE NUMBERS (clock
#                                        constants that happened to live next door).
#                                        A silent wrong answer with real data in it.
#
#   ping_pong      -> HUNG the machine.  ⭐ AND THAT WAS MY OWN BUG, NOT THE MODEL'S:
#                                        I "fixed" scatter-gather by RE-ARMING the
#                                        channel to walk the TCD chain in hardware.
#                                        Real eDMA LOADS the next TCD and STOPS -- the
#                                        channel must be re-triggered.  A ping-pong
#                                        chain is CIRCULAR by design and the NXP driver
#                                        walks it from its own ISR, so my chaining
#                                        double-processed it and the machine spun.
#                                        MUTATION TESTING FOUND IT: disabling
#                                        scatter-gather ENTIRELY left the test green AND
#                                        made ping_pong pass.  A feature I added, that
#                                        was not needed, that broke a working case --
#                                        and only breaking it on purpose showed me.
#
# ⭐ AND THE LAST ONE WAS A CORE SEMANTICS BUG: TCD_CSR[START] is a SERVICE REQUEST --
# it runs ONE MINOR LOOP, not the whole major loop.  channel_link calls
# EDMA_TriggerChannelStart TWICE precisely because its channel has CITER = 2.  My
# model finished the entire transfer on the first trigger, so the second trigger RAN
# EVERYTHING AGAIN with the linked channels' addresses already advanced -- writing
# PAST their buffers, corrupting guest memory, and HARD-FAULTING the CPU (CFSR =
# INVSTATE, HFSR = FORCED).  The "hang" was never a hang: THE GUEST HAD CRASHED, and
# HardFault_Handler is a while(1).  Only gdb showed it -- every trace I had said the
# transfer worked and the interrupt fired, and both were TRUE.
#
# SKIPs (does not fail) when the MCUXpresso workspace is not staged.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
WS="${MCUX_WS:-$HOME/mcux-ws}"
BASE="$WS/examples/frdmmcxn947/driver_examples/edma3"

[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
[ -d "$BASE" ] || { echo "SKIP: MCUXpresso edma3 examples not staged"; exit 0; }

check() {
    local ex="$1" want="$2" elf out ser
    elf=$(find "$BASE/$ex" -name '*.elf' 2>/dev/null | head -1)
    [ -n "$elf" ] || { echo "  SKIP  $ex (not built)"; return 0; }
    ser=$(mktemp)
    timeout -k 5 15 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial "file:$ser" -kernel "$elf" -no-reboot >/dev/null 2>&1 || true
    out=$(tr '\r' '\n' < "$ser" | grep -vE '^[[:space:]]*$' | tail -1)
    rm -f "$ser"
    if ! echo "$out" | grep -qE "$want"; then
        echo "  FAIL  $ex"
        echo "        want: $want"
        echo "        got : $out"
        return 1
    fi
    echo "  ok    $ex"
    return 0
}

fail=0
check memory_to_memory    '^1[[:space:]]+2[[:space:]]+3[[:space:]]+4'  || fail=1
check memset              '^1[[:space:]]+1[[:space:]]+1[[:space:]]+1'  || fail=1
check interleave_transfer '^1[[:space:]]+0[[:space:]]+2[[:space:]]+0'  || fail=1
check scatter_gather      '^1[[:space:]]+2[[:space:]]+3[[:space:]]+4[[:space:]]+5[[:space:]]+6[[:space:]]+7[[:space:]]+8' || fail=1
check wrap_transfer       '^1[[:space:]]+2[[:space:]]+3[[:space:]]+4[[:space:]]+1[[:space:]]+2[[:space:]]+3[[:space:]]+4' || fail=1
check ping_pong_transfer  '^1[[:space:]]+2[[:space:]]+3[[:space:]]+4[[:space:]]+5[[:space:]]+6[[:space:]]+7[[:space:]]+8' || fail=1
check channel_link        '^1[[:space:]]+2[[:space:]]+3[[:space:]]+4' || fail=1

[ $fail -eq 0 ] && { echo "PASS: the stock NXP eDMA examples move the RIGHT DATA"; exit 0; }
echo "FAIL"; exit 1
