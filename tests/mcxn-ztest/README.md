# mcxn-ztest — Zephyr kernel test suites on the model

Validates `frdm-mcxn947` against **Zephyr's own ztest framework** — third-party
firmware exercising the kernel, not our hand-written device tests. Each suite
prints `PROJECT EXECUTION SUCCESSFUL` on the FlexComm4 console when every case
passes; `run.sh` asserts that string for each staged ELF.

## What it exercises

| Suite | Coverage |
|-------|----------|
| `tests/kernel/context` | IRQ enable/lock/unlock, context switching, `k_cpu_idle` (WFI), busy-wait, `k_sleep`, `k_yield`, thread creation |
| `tests/kernel/fifo/fifo_api` | FIFO IPC: put/get, blocking, timeouts, cancel |
| `tests/kernel/lifo/lifo_api` | LIFO IPC: put/get, blocking, timeouts |
| `tests/kernel/mbox/mbox_api` | mailbox IPC: synchronous/async message passing |
| `tests/kernel/mem_slab/mslab_api` | fixed-block memory slab alloc/free |

These build **without** `CONFIG_USERSPACE`, so no `gperf` host tool is needed.
Userspace/MPU suites (`tests/kernel/common`, `mem_protect/*`) additionally need
`gperf` (`sudo apt-get install -y gperf`) — worth adding to cover the SAU/MPU
path.

**Excluded on purpose:** HW timing-accuracy suites such as
`tests/kernel/timer/timer_behavior`. Their jitter/drift/ramp asserts demand
cycle-accurate timing (e.g. "32768 ticks must land within ±2"), which QEMU's
TCG cannot provide — they fail on any emulator. Zephyr's own twister filters
them to real hardware. Functional timing (`k_sleep`, timeouts) is covered by
the suites above and passes.

## Build + run

The ELFs are operator-built and kept out of the repo (Apache-2.0, but consistent
with the other Zephyr/MCUXpresso harnesses). With a Zephyr workspace at
`~/zephyrproject` (west + gnuarmemb, the same env as `../mcxn-zephyr`):

```sh
tests/mcxn-ztest/build.sh     # builds the suites, stages ~/mcxn-images/ztest/*.elf
tests/mcxn-ztest/run.sh       # boots each on the model, asserts PROJECT EXECUTION SUCCESSFUL
```

`run.sh` SKIPs cleanly when no ELFs are staged. Override the ELF directory with
`ZTEST_DIR=` and the QEMU binary with `QEMU=`.
