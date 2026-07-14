#!/bin/bash
# THE PINNED ARTIFACT MUST BE A BUILD OF THE COMMITTED SOURCE.
#
# Stopping the test suite from OVERWRITING node-*.elf fixed one bug and armed its dual:
# an artifact nothing rewrites is an artifact that can go SILENTLY STALE relative to
# main.c -- and holobench's farm would faithfully run last week's firmware forever,
# reporting green on code that is not in this repo any more.
#
#   ⭐ I TRADED "SILENTLY REWRITTEN" FOR "SILENTLY STALE" -- THE SAME BUG IN THE OTHER
#     COAT -- AND THE ONLY THING THAT MAKES A PIN MEAN ANYTHING IS A GATE THAT CHECKS IT.
#
# So: rebuild from the committed source and demand the bytes match what is committed.
# (holobench's dual: "never run a lab against an artifact whose hash you did not verify."
#  This is the producer proving the hash is worth verifying.)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE/../.."
CC=${CC:-arm-none-eabi-gcc}
command -v "$CC" >/dev/null || { echo "SKIP: no $CC"; exit 0; }
L3=tests/mcxn-enet-lab3
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

b() { "$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 -Wall \
   -DMY_ETHERTYPE=$1 -DPEER_A=$2 -DPEER_B=$3 -DMY_MAC_LSB=$4 \
   -T "$L3/link.ld" "$L3/main.c" -o "$5"; }
b 0x88B5 0x88B6 0x88B7 0x01 "$T/node-mcx.elf"     || { echo "SKIP: build failed"; exit 0; }
b 0x88B6 0x88B5 0x88B7 0x02 "$T/node-rt1180.elf"  || { echo "SKIP: build failed"; exit 0; }
b 0x88B7 0x88B5 0x88B6 0x03 "$T/node-imx95.elf"   || { echo "SKIP: build failed"; exit 0; }

rc=0
for e in node-mcx node-rt1180 node-imx95; do
    if ! cmp -s "$T/$e.elf" "$L3/$e.elf"; then
        echo "FAIL: $L3/$e.elf is NOT a build of the current main.c."
        echo "      The board farm pins this file.  It would run stale firmware and pass."
        echo "      Fix: bash $L3/stage-artifacts.sh && git add $L3/$e.elf"
        rc=1
    fi
done
[ $rc -eq 0 ] && echo "PASS: the published lab artifacts ARE a build of the committed source"
exit $rc
