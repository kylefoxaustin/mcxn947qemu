#!/usr/bin/env bash
# Boot a real Zephyr hello_world for frdm_mcxn947 on the model and assert the
# banner. CI-safe: skips when no Zephyr ELF is available.
#
# Build the ELF with (in a Zephyr workspace):
#   west build -b frdm_mcxn947/mcxn947/cpu0 samples/hello_world
# Override path with ZEPHYR_ELF=/path/to/zephyr.elf
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
ZEPHYR_ELF="${ZEPHYR_ELF:-$HOME/zephyrproject/zephyr/build/zephyr/zephyr.elf}"
[ -x "$QEMU" ] || { echo "SKIP: qemu not built at $QEMU"; exit 0; }
[ -f "$ZEPHYR_ELF" ] || { echo "SKIP: no Zephyr ELF at $ZEPHYR_ELF"; exit 0; }
OUT="$(timeout 12 "$QEMU" -M frdm-mcxn947 -display none -monitor none \
        -serial stdio -kernel "$ZEPHYR_ELF" -no-reboot 2>/dev/null || true)"
echo "--- guest output ---"; echo "$OUT"; echo "--------------------"
fail=0
echo "$OUT" | grep -q "Booting Zephyr OS" || { echo "FAIL: no Zephyr boot banner"; fail=1; }
echo "$OUT" | grep -q "Hello World" || { echo "FAIL: no Hello World"; fail=1; }
[ $fail -eq 0 ] && echo "PASS: Zephyr booted on frdm-mcxn947"
exit $fail
