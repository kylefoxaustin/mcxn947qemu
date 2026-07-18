#!/usr/bin/env bash
# MCXN947 USBDCD: the BC1.2 charger-detection SEQUENCE, swept across every attached port.
#
# The block runs the real detection phases (contact -> primary -> secondary) and reports
# the classification; what is on the port is operator-driven (-global mcxn-usbdcd.charger),
# never fabricated.  We run the SAME firmware against all four ports and demand it classify
# each -- so the register-only model that stamped one fixed answer fails three of four.
#   none -> NONE (contact-detect timeout)   SDP -> SDP   CDP -> CDP   DCP -> DCP
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/usbdcd.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

run_charger() {  # <charger-value> -> prints the DCD RESULT line
    timeout -k 5 10 "$QEMU" -M frdm-mcxn947 -display none -monitor none -serial stdio \
        -global mcxn-usbdcd.charger="$1" -kernel "$ELF" -no-reboot 2>/dev/null |
        grep -oE "DCD RESULT: [A-Z]+" | head -1
}

rc=0
check() {  # <charger> <expected>
    local got; got="$(run_charger "$1")"
    printf "  charger=%s -> %-24s (want %s)\n" "$1" "${got:-<none>}" "DCD RESULT: $2"
    [ "$got" = "DCD RESULT: $2" ] || { echo "    FAIL"; rc=1; }
}

check 0 NONE
check 1 SDP
check 2 CDP
check 3 DCP

[ $rc -eq 0 ] && { echo "PASS"; exit 0; } || { echo "FAIL"; exit 1; }
