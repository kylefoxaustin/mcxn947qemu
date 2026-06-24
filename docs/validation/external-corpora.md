# External MCXN947 code corpora for functional validation

Sources of real, runnable MCX code with a *documented expected output*, so the
QEMU model can be checked against firmware authored by someone other than us.
Pass/fail is a console grep unless noted.  Console on FRDM-MCXN947 = FlexComm4 /
LPUART4 @ 115200 8N1 (matches the model).

Already in active use: NXP `mcux-sdk-examples` (frdmmcxn947 tree, ~155 cm33
examples — see `mcuxpresso-corpus-scorecard.tsv`); Zephyr `frdm_mcxn947`
(hello_world, blinky, net/dhcpv4_client); the MCUXpresso SDK drivers.

## Tier 1 — turnkey, deterministic, greppable (highest leverage)

| Source | Clone / path | License | MCXN947 maturity | Pass grep | Toolchain |
|---|---|---|---|---|---|
| **TF-M** (Trusted Firmware-M) | `zephyrproject-rtos/trusted-firmware-m`, `platform/ext/target/nxp/frdmmcxn947/` (also present under `~/zephyrproject/modules/tee/tf-m`) | BSD-3 | In-tree, dual-CM33 S/NS, native flash+LPUART. No BL2 yet. | `PASSED` per suite; `*** End of Non-secure test suites ***` | CMake + arm-none-eabi-gcc, `-DTFM_PLATFORM=nxp/frdmmcxn947` |
| **Zephyr ztest** `tests/kernel/*`, `tests/lib/*` | `zephyrproject-rtos/zephyr`, `tests/...` | Apache-2.0 | cpu0 supports adc/can/dma/flash/gpio/i2c/i2s/i3c/eth/pwm/rtc/spi/usb/wdt | `PROJECT EXECUTION SUCCESSFUL` | `twister -p frdm_mcxn947/mcxn947/cpu0` |
| Zephyr `samples/synchronization` | same | Apache-2.0 | in-tree | `thread_a: Hello World from cpu 0 on frdm_mcxn947/mcxn947/cpu0!` | west |
| Zephyr `samples/philosophers` | same | Apache-2.0 | in-tree | `.*EATING.*` / `.*THINKING.*` lines | west |

## Tier 2 — runtimes riding the Zephyr board (≈free once Zephyr boots)

| Source | Path | License | Pass grep |
|---|---|---|---|
| **MicroPython** | `micropython/ports/zephyr` board `frdm_mcxn947_mcxn947_cpu0` | MIT | `MicroPython v... zephyr-frdm_mcxn947 with mcxn947` + `>>>` |
| **CircuitPython** | `adafruit/circuitpython`, board `nxp_frdm_mcxn947` (prebuilt at circuitpython.org) | MIT | `Adafruit CircuitPython ... nxp_frdm_mcxn947` + `>>>` |

## Tier 3 — dual-core (relevant to the cpu1 to-do)

- Zephyr multicore samples (in-tree): `samples/drivers/mbox`, `.../mbox_data`,
  `samples/subsys/ipc/ipc_service/static_vrings`, `samples/subsys/ipc/openamp`.
  OpenAMP golden: `OpenAMP[master] demo started`, `Master core received a
  message: N`, `OpenAMP demo ended.`  Build `--sysbuild`. Apache-2.0.
- `nxp-appcodehub/dm-dual-core-i2c-communication-on-mcxn947` — core0 LPI2C3
  master ↔ core1 LPI2C7 slave; red LED toggles per transfer. NOASSERTION.

## Tier 4 — benchmarks with known numeric output

- **EEMBC CoreMark** (`eembc/coremark`, Apache-2.0) — NOT in the NXP examples
  git; port it. Deterministic CRCs independent of timing: `seedcrc 0xe9f5`,
  `crclist 0xe714`, `crcmatrix 0x1fd7`, `crcstate 0x8e3a`, `crcfinal 0x33ff`,
  `Correct operation validated`. NXP-published score ≈ 618 CoreMark/core
  (4.12 CoreMark/MHz @ 150 MHz). Methodology: NXP AN12387 / AN12284.
- Zephyr `tests/benchmarks/thread_metric` (EEMBC Thread-Metric, ported) —
  numeric, deterministic only against a fixed QEMU clock model.

## Tier 5 — NXP App Code Hub (`github.com/nxp-appcodehub/<repo>`, mostly NOASSERTION)

- `dm-mcxn947-power-manager` — golden menu banner
  `#######    MCX Nx4x Power Manager component demo    #######` (interactive,
  good UART RX/TX exercise).
- `an-using-flexio-to-generate-center-aligned-pwm-on-mcxn947` — clean BSD-3
  (redistributable); PWM-on-pin (scope, not serial).
- `dm-mcxn947-npu-vs-tensorflm-benchmark` — relevant to the Neutron NPU roadmap.

## Confirmed NOT supported (don't spend build time)

NuttX (only MCXN236, no N947) · TinyGo (no MCX-N) · Rust embassy (`embassy-mcxa`
is MCX-A only; only solid artifact is the PAC `embassy-rs/nxp-pac` / `mcxn947-pac`
— register-only, useful as a CMSIS offset cross-check) · Arduino core · Azure
RTOS/ThreadX (discontinued) · Mbed OS.

## Gotchas

- NXP `demo_apps/hello_world` prints `hello world.` (lowercase, period) —
  DIFFERENT from the Zephyr banner `Hello World! frdm_mcxn947/mcxn947/cpu0`.
- The NXP examples corpus has **no** CoreMark for frdmmcxn947.

## Recommended add-on validation order

1. Zephyr `tests/kernel/*` via twister → `PROJECT EXECUTION SUCCESSFUL`
   (large deterministic functional suite — biggest single coverage win).
2. TF-M regression → exercises SAU/MPU/secure-alias at `0x500x_xxxx`, which
   nothing else does.
3. Zephyr `synchronization` + `philosophers`; OpenAMP once cpu1 is up.
4. EEMBC CoreMark (ported) → CRC strings + score vs ≈618/core.
