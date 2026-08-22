#!/usr/bin/env bash
# MCXN947 migrate-mid-interrupt regression (pre-upstream Fable item #5): a level
# IRQ that is asserted at save time must be RE-DRIVEN from restored register
# state by a vmstate post_load, or the destination lands with the line low and
# the guest's interrupt is silently lost.
#
# The firmware (main.c) holds the DAC FIFO-empty level IRQ high and re-arms it at
# the NVIC every loop, so its "fires" counter only keeps climbing while the line
# is driven high.  We migrate mid-run and assert fires KEEPS CLIMBING on the
# destination -- so a live level IRQ survives the round-trip.  It catches a
# broken/missing vmstate for the IRQ-determining registers (mutation-checked:
# force the DAC IRQ to deassert -> no fires -> FAIL).  Note it does NOT isolate
# the peripheral post_load: the ARMv7M NVIC also saves each input line's level,
# which alone covers NVIC-connected IRQs (the post_loads are belt-and-suspenders
# and the sole defence for non-NVIC device-to-device level outputs).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
QEMU="${QEMU:-$HERE/../../build/qemu-system-arm}"
CC="${CC:-arm-none-eabi-gcc}"
ELF="$HERE/migrate-irq.elf"
command -v "$CC" >/dev/null 2>&1 || { echo "SKIP: $CC not found"; exit 0; }
[ -x "$QEMU" ] || { echo "SKIP: qemu not built"; exit 0; }
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -nostartfiles -ffreestanding -O2 \
      -Wall -T "$HERE/link.ld" "$HERE/main.c" -o "$ELF"

MIG="$(mktemp)"; POST="$(mktemp)"
trap 'rm -f "$MIG" "$POST"' EXIT

# Save: boot, let the IRQ fire a while (line high), migrate full state, quit.
( sleep 3; printf 'migrate "exec:cat > %s"\n' "$MIG"; sleep 2; printf 'quit\n' ) | \
  timeout -k 5 15 "$QEMU" -M frdm-mcxn947 -display none -serial null -monitor stdio \
  -kernel "$ELF" -no-reboot >/dev/null 2>&1 || true
[ -s "$MIG" ] || { echo "FAIL: migration file empty (save failed)"; exit 1; }

# Restore: resume into a fresh instance, capture the resumed console.
timeout -k 5 8 "$QEMU" -M frdm-mcxn947 -display none -serial "file:$POST" -monitor none \
  -incoming "exec:cat $MIG" -kernel "$ELF" -no-reboot >/dev/null 2>&1 || true

# On the destination, does the fire counter keep CLIMBING (level re-driven)?
# Use only COMPLETE 8-digit prints (a timeout can truncate the last line), and
# sort them: zero-padded hex sorts lexically == numerically, so lo != hi means
# the counter advanced after resume.  Frozen (no re-drive) => a single value.
vals="$(grep -oE 'fires=[0-9a-f]{8}' "$POST" 2>/dev/null | sed 's/fires=//' | sort)"
n="$(printf '%s\n' "$vals" | grep -c .)"
lo="$(printf '%s\n' "$vals" | head -1)"
hi="$(printf '%s\n' "$vals" | tail -1)"
echo "resumed: $n samples, fire-count lo=$lo hi=$hi"
if [ "$n" -ge 2 ] && [ "$lo" != "$hi" ]; then
    echo "PASS: DAC level IRQ re-driven after migration (fires kept climbing)"
    exit 0
else
    echo "FAIL: fires frozen after migration -> IRQ line not re-driven (missing post_load)"
    exit 1
fi
