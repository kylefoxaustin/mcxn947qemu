#!/usr/bin/env bash
# Boot the MCUXpresso SDK hello_world on frdm-mcxn947 and assert the SDK driver
# path reaches PRINTF over the FlexComm4/LPUART4 console.
#
# Redistribution: the MCUXpresso SDK binary is NOT shipped in this repo.  This
# test SKIPS cleanly when no ELF is staged.  Provide one of:
#   * a prebuilt ELF at $MCUX_ELF (default: ~/mcux-build/mcux-hello.elf), or
#   * a local mcux-sdk checkout so build.sh can build it on demand (see README).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
ELF="${MCUX_ELF:-$HOME/mcux-build/mcux-hello.elf}"

[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }

if [ ! -f "$ELF" ]; then
  # Try to build it from a local SDK checkout; skip if the SDK/toolchain absent.
  if [ -d "${MCUX_SDK:-$HOME/mcux-sdk}/devices/MCXN947" ] && command -v "${CC:-arm-none-eabi-gcc}" >/dev/null; then
    ELF="$HERE/mcux-hello.elf"
    OUT="$ELF" "$HERE/build.sh" >/dev/null 2>&1 || { echo "SKIP: SDK build failed"; exit 0; }
  else
    echo "SKIP: no MCUXpresso ELF staged and no local mcux-sdk to build from"
    exit 0
  fi
fi

OUT=$(mktemp); trap 'rm -f "$OUT"' EXIT
timeout -s KILL 10 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
  -serial "file:$OUT" -kernel "$ELF" -no-reboot >/dev/null 2>&1

if grep -q 'MCUXPRESSO-SDK-PASS' "$OUT"; then
  echo "PASS: MCUXpresso SDK driver path (startup+clock+lpflexcomm/lpuart+console) booted"
  exit 0
fi
echo "FAIL: SDK banner not seen"; cat "$OUT"; exit 1
