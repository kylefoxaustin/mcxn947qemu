#!/usr/bin/env bash
#
# PAD RESET VALUES — AND THEY ARE PER-PORT, WHICH IS WHY THE MAIN GATE CANNOT SEE THEM.
#
# tests/mcxn-reset-values stores ONE reset per (name, offset) and SHARES IT ACROSS
# INSTANCES.  The RM's PCR rows say "See section" in the reset column, and the section
# says why:
#
#     Register reset values
#     Register    Reset value
#     PCR0        PORT0:        0000_1143h
#                 PORT1-PORT5:  0000_0000h
#
#     ⭐ THE RESET VALUE DEPENDS ON THE INSTANCE.  The extractor CORRECTLY REFUSED the
#        row rather than pick one of the two values -- a missing register is a gap; a
#        WRONG one is a false witness.  But a refusal is not a check, so the registers
#        it refused need one, and this is it.  HAND-READ FROM THE RM, and the numbers
#        below appear nowhere in the model.
#
# WHAT IT COSTS, AND IT IS NOT COSMETIC.  PORT0's PCR0/PCR3/PCR6 carry MUX=1 with pulls
# enabled out of reset -- THEY ARE THE SWD DEBUG PINS.  We reset them to ZERO.  The SDK's
# PORT_SetPinConfig does a READ-MODIFY-WRITE on the pad, so firmware touching a
# neighbouring field READS A VALUE THE SILICON WOULD NEVER PRODUCE and writes back a pad
# configuration that never existed -- silently clobbering the debug pin's mux and pull.
#
# (91emulator found the identical class on their pinmux, where a wrapped table cell had
# hidden the whole block: "any driver doing a read-modify-write on a pad reads a value
# the silicon would never produce."  Same bug, different manual, different parser hole.)
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }

# CMSIS: PORT0_BASE = 0x40116000 ... PORT5_BASE = 0x40042000;  PCR[n] at 0x80 + 4n.
declare -A BASE=( [PORT0]=0x40116000 [PORT1]=0x40117000 [PORT5]=0x40042000 )

# port pcr expected   -- HAND-READ FROM THE RM, not from the model.
CASES=(
  "PORT0 0 0x00001143"     # SWD
  "PORT0 3 0x00001103"     # SWD
  "PORT0 6 0x00001103"     # SWD
  "PORT5 2 0x00000100"
  "PORT1 0 0x00000000"     # control: PORT1 shares the offset and resets to ZERO
  "PORT0 1 0x00000000"     # control: a neighbouring pad on the SAME port is ZERO
)

QS="$(mktemp)"; trap 'rm -f "$QS"' EXIT
for c in "${CASES[@]}"; do
    set -- $c
    printf 'readl 0x%x\n' $(( ${BASE[$1]} + 0x80 + 4 * $2 )) >> "$QS"
done

mapfile -t V < <(timeout -k 5 60 "$QEMU" -M frdm-mcxn947 -display none -accel qtest \
                     -qtest stdio -monitor none -serial none < "$QS" 2>/dev/null \
                 | grep -oE '^OK 0x[0-9a-f]+' | cut -d' ' -f2)

[ "${#V[@]}" -eq "${#CASES[@]}" ] || {
    echo "FAIL: asked ${#CASES[@]} questions, got ${#V[@]} answers"; exit 1; }

fail=0
for i in "${!CASES[@]}"; do
    set -- ${CASES[$i]}
    got=$(printf '%d' "${V[$i]}"); want=$(printf '%d' "$3")
    if [ "$got" -eq "$want" ]; then
        printf '  PASS  %-6s PCR%-2s = 0x%08x\n' "$1" "$2" "$got"
    else
        printf '  FAIL  %-6s PCR%-2s = 0x%08x, RM says 0x%08x\n' "$1" "$2" "$got" "$want"
        fail=1
    fi
done
[ $fail -eq 0 ] && echo "PASS: per-port pad reset values match the RM (incl. the SWD pins)"
exit $fail
