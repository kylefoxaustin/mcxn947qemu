#!/usr/bin/env bash
# Build a set of Zephyr ztest suites for frdm_mcxn947/mcxn947/cpu0 and stage the
# ELFs where run.sh looks for them ($HOME/mcxn-images/ztest/<suite>.elf).
#
# Requires a Zephyr workspace (default ~/zephyrproject) with west + the gnuarmemb
# toolchain, the same environment that builds the samples in ../mcxn-zephyr.
# Zephyr is Apache-2.0; the ELFs are still kept out of the repo (operator-built),
# consistent with the other Zephyr/MCUXpresso test harnesses.
#
# The userspace/MPU suites (mem_protect/*, semaphore, queue, poll, ...) need the
# `gperf` host tool for kobject-hash generation: `sudo apt-get install -y gperf`.
set -euo pipefail
ZDIR="${ZEPHYRDIR:-$HOME/zephyrproject/zephyr}"
VENV="${ZEPHYR_VENV:-$HOME/zephyrproject/.venv/bin/activate}"
OUT="${ZTEST_DIR:-$HOME/mcxn-images/ztest}"
BOARD="frdm_mcxn947/mcxn947/cpu0"

# suite path under tests/ -> staged ELF basename.
# Functional kernel + userspace/MPU suites.  HW timing-accuracy suites (e.g.
# kernel/timer/timer_behavior) are intentionally excluded: their jitter/drift
# asserts require cycle-accurate timing that QEMU TCG cannot provide (Zephyr's
# own twister filters them to hardware) — they fail on any emulator, not just
# this model.
SUITES=(
    # functional kernel (no userspace)
    "kernel/context:context"
    "kernel/fifo/fifo_api:fifo_api"
    "kernel/lifo/lifo_api:lifo_api"
    "kernel/mbox/mbox_api:mbox_api"
    "kernel/mem_slab/mslab_api:mslab_api"
    # userspace / MPU / SAU secure path (need gperf)
    "kernel/mem_protect/userspace:userspace"
    "kernel/mem_protect/mem_protect:mem_protect"
    "kernel/mem_protect/syscalls:syscalls"
    "kernel/mem_protect/protection:protection"
    "kernel/semaphore/semaphore:semaphore"
    "kernel/mutex/sys_mutex:sys_mutex"
    "kernel/queue:queue"
    "kernel/poll:poll"
)

# shellcheck disable=SC1090
source "$VENV"
export ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb GNUARMEMB_TOOLCHAIN_PATH=/usr
mkdir -p "$OUT"
cd "$ZDIR"

for entry in "${SUITES[@]}"; do
    path="tests/${entry%%:*}"; name="${entry##*:}"
    bd="$(mktemp -d)"
    echo "=== building $path ==="
    west build -p always -b "$BOARD" "$path" -d "$bd"
    cp "$bd/zephyr/zephyr.elf" "$OUT/$name.elf"
    rm -rf "$bd"
    echo "staged $OUT/$name.elf"
done
echo "Done. Run: tests/mcxn-ztest/run.sh"
