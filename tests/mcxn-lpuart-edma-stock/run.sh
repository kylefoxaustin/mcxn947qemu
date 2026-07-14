#!/usr/bin/env bash
#
# THE REAL TENANT.  Runs NXP's OWN unmodified SDK example --
# driver_examples/lpuart/edma_transfer, i.e. LPUART_TransferReceiveEDMA +
# LPUART_TransferSendEDMA -- against the model, feeds it 8 characters, and
# asserts they come back.
#
# ⭐ WHY THIS EXISTS, AND WHY tests/mcxn-uart-dma IS NOT ENOUGH.
#
# mcxn-uart-dma proves THE MODEL REACTS TO THE SIGNAL I SEND IT.  It is firmware I
# wrote, poking the registers I chose.  It does NOT prove that the REAL DRIVER
# PRODUCES THAT SIGNAL -- and those are different claims, only the second of which
# matters to a developer.
#
# qualcomm and claude-connect found this the expensive way, on hardware: a board
# guard was negative-tested against a MOCK tenant and passed, but a REAL tenant
# reached the device by mmap'ing the PCI BAR -- which never touched the signal the
# guard was reading.  The guard would have reported "free" on a board running
# inference at full tilt.
#
#     "The mock proved the script REACTS TO THE SIGNAL.  It never proved A REAL
#      TENANT PRODUCES THE SIGNAL.  Those are different claims, and only the
#      second one matters."
#     ⇒ A guard negative-tested only against a MOCK is decoration until a REAL
#       TENANT trips it.  Go and hold the thing.  Watch the check fire.
#
# So: the stock driver.  On this example the echo can ONLY come back through the
# eDMA path -- the banner is printed with blocking writes and proves nothing.  If
# the FlexComm DMA request lines are wrong, the characters go in and NOTHING comes
# out.
#
# SKIPs (does not fail) when the MCUXpresso workspace is not staged.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
WS="${MCUX_WS:-$HOME/mcux-ws}"
EX="$WS/examples/frdmmcxn947/driver_examples/lpuart/edma_transfer"
ELF="$EX/cm33_core0/armgcc/debug/lpuart_edma_transfer.elf"

[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
[ -d "$EX"   ] || { echo "SKIP: MCUXpresso example not staged ($EX)"; exit 0; }
if [ ! -f "$ELF" ]; then
    ( cd "$WS" && bash run-example.sh driver_examples/lpuart/edma_transfer "" 5 ) \
        >/dev/null 2>&1 || true
fi
[ -f "$ELF" ] || { echo "SKIP: could not build the stock example"; exit 0; }

MSG="ABCDEFGH"     # the example echoes every 8 characters
OUT="$( (printf '%s' "$MSG"; sleep 6) | timeout -k 5 20 "$QEMU" -M frdm-mcxn947 \
        -display none -monitor none -serial stdio -kernel "$ELF" -no-reboot \
        2>/dev/null || true )"

echo "--- stock NXP LPUART_Transfer*EDMA example ---"
echo "$OUT"
echo "---------------------------------------------"

echo "$OUT" | grep -q "LPUART EDMA example" || {
    echo "FAIL: the stock example did not even start"; exit 1; }

# The echo can ONLY arrive via LPUART_TransferReceiveEDMA -> SendEDMA.
if echo "$OUT" | grep -q "$MSG"; then
    echo "PASS: the REAL NXP driver's eDMA echo works ($MSG went in and came back)"
    exit 0
fi
echo "FAIL: the stock driver's eDMA path did not echo -- the FlexComm DMA request"
echo "      lines do not produce the signal the REAL driver waits on."
exit 1
