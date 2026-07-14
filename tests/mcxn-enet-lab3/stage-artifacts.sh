#!/bin/bash
# Refresh the PINNED lab artifacts (node-*.elf) and print their hashes.
#
# These ELFs are what holobench's board farm CONSUMES.  They are deliberate published
# artifacts, NOT build outputs -- `run.sh` builds into a temp dir and never touches them.
# Run this ONLY when you intend to publish a new node, then COMMIT the result and
# ANNOUNCE the commit SHA + md5s on the bus.
#
#   ⭐ A COMMITTED ARTIFACT THAT A TEST OVERWRITES IS NOT A PINNED ARTIFACT.
#     (It used to be: every suite run rewrote these, so the farm got whatever my last
#      test left behind.  holobench's staged copy was TWO GENERATIONS STALE.)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CC=${CC:-arm-none-eabi-gcc}
command -v "$CC" >/dev/null || { echo "no $CC"; exit 1; }

b() { "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall ${6:-} \
   -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DMY_MAC_LSB=$4 \
   -T "$HERE/link.ld" "$HERE/main.c" -o "$5"; }
# ⭐ THE PUBLISHED NODE JOINS A FOUR-NODE LAB, SO IT MUST KNOW THE FOURTH PEER.
#   imx91 (0x88B8) was invisible to it: is_beacon_et() said "not my protocol" and
#   frame_ok() never read a byte of their body.  "mcx never rejected imx91" and
#   "mcx never LOOKED at imx91" were the same cell in holobench's matrix.
b 0x88B5 0x88B6 0x88B7 0x01 "$HERE/node-mcx.elf" -DPEER_C=0x88B8
b 0x88B6 0x88B5 0x88B7 0x02 "$HERE/node-rt1180.elf"
b 0x88B7 0x88B5 0x88B6 0x03 "$HERE/node-imx95.elf"

echo "staged from $(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo '?'):"
for e in node-mcx node-rt1180 node-imx95; do
    printf "  %-16s md5 %s\n" "$e.elf" "$(md5sum "$HERE/$e.elf" | cut -d' ' -f1)"
done
echo "COMMIT these, then announce the SHA + md5s on the bus."
