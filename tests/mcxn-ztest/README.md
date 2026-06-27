# mcxn-ztest — Zephyr kernel test suites on the model

Validates `frdm-mcxn947` against **Zephyr's own ztest framework** — third-party
firmware exercising the kernel, not our hand-written device tests. Each suite
prints `PROJECT EXECUTION SUCCESSFUL` on the FlexComm4 console when every case
passes; `run.sh` asserts that string for each staged ELF.

## What it exercises

### Functional kernel (no `CONFIG_USERSPACE`)

| Suite | Coverage |
|-------|----------|
| `tests/kernel/common` | comprehensive kernel sanity (atomics, bitfields, clock, errno, multilib, slist/dlist, …) |
| `tests/kernel/context` | IRQ enable/lock/unlock, context switching, `k_cpu_idle` (WFI), busy-wait, `k_sleep`, `k_yield`, thread creation |
| `tests/kernel/fifo/fifo_api` | FIFO IPC: put/get, blocking, timeouts, cancel |
| `tests/kernel/lifo/lifo_api` | LIFO IPC: put/get, blocking, timeouts |
| `tests/kernel/mbox/mbox_api` | mailbox IPC: synchronous/async message passing |
| `tests/kernel/mem_slab/mslab_api` | fixed-block memory slab alloc/free |
| `tests/kernel/mutex/mutex_api` | mutex API: lock/unlock, priority inheritance, recursion |
| `tests/kernel/pipe/pipe_api` | pipe IPC: byte-stream put/get, blocking, timeouts |
| `tests/kernel/workq/work_queue` | workqueue: submit, delayed work, cancellation |
| `tests/kernel/timer/timer_api` | timer API: start/stop, period, expiry/stop callbacks, status |
| `tests/kernel/condvar/condvar_api` | condition variables: wait/signal/broadcast |
| `tests/kernel/sleep` | `k_sleep`/`k_usleep` timing + busy-wait |
| `tests/kernel/tickless/tickless_concept` | tickless-idle SysTick reprogramming |

### Userspace / MPU / SAU secure path (need `gperf`)

These run threads in unprivileged mode behind the MPU and cross the user→kernel
boundary via SVC — the only suites that exercise the model's MPU enforcement and
secure handling. Install the host tool first: `sudo apt-get install -y gperf`.

| Suite | Coverage |
|-------|----------|
| `tests/kernel/mem_protect/userspace` | privilege transitions, memory-domain isolation, read/write/exec permission faults |
| `tests/kernel/mem_protect/mem_protect` | memory-domain partition add/remove/enforce |
| `tests/kernel/mem_protect/syscalls` | user→kernel syscall boundary + argument validation |
| `tests/kernel/mem_protect/protection` | fault-on-violation (RO write, NULL deref, exec data) |
| `tests/kernel/mem_protect/futex` | userspace futex wait/wake |
| `tests/kernel/mem_protect/obj_validation` | kernel-object permission validation |
| `tests/kernel/mem_protect/stack_random` | stack-pointer randomisation (exercises the ELS TRNG entropy path) |
| `tests/kernel/semaphore/semaphore` | counting semaphores from user threads |
| `tests/kernel/mutex/sys_mutex` | userspace mutex API |
| `tests/kernel/queue` | k_queue from user threads |
| `tests/kernel/poll` | k_poll multi-object wait from user threads |

24 suites in total, ~360 ztest cases, all green on the model.

**Excluded on purpose** (depend on capabilities the model doesn't provide — not model bugs):
- `tests/kernel/timer/timer_behavior` — HW timing-accuracy (jitter/drift/ramp)
  asserts demand cycle-accurate timing (e.g. "32768 ticks within ±2") that QEMU
  TCG cannot provide; fails on any emulator. Zephyr's twister filters it to
  hardware. Functional timing (`k_sleep`, timeouts, `timer_api`) is covered and
  passes.

(`mem_protect/stack_random` was previously excluded for lack of entropy; the ELS
TRNG data output is now functionally modelled, so it passes and is included.)

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
