# MCUXpresso SDK BSP smoke test (frdm-mcxn947)

Second, independent firmware path for the model — the **NXP MCUXpresso SDK**
driver stack, alongside the Zephyr tests. Proves the model satisfies the stock
NXP drivers, not just Zephyr's.

What it exercises (stock NXP SDK, unmodified):

- `startup_MCXN947_cm33_core0.S` + `SystemInit` (system_MCXN947)
- `fsl_clock` — `BOARD_BootClockFRO12M` (12 MHz FRO)
- `fsl_lpflexcomm` + `fsl_lpuart` — `LP_FLEXCOMM_Init` → `LPUART_Init`
- `debug_console_lite` + `fsl_str` — `PRINTF` over FlexComm4 / LPUART4

Success = the console prints `MCUXPRESSO-SDK-PASS`.

## Redistribution

The MCUXpresso SDK **source** is BSD-3-Clause (build it locally), but built
**binaries are not redistributed here**. No ELF is committed; `run.sh` skips
cleanly when none is staged. This mirrors the operator-supplied-firmware rule
used for the MCUXpresso path elsewhere in the project.

## Building the firmware

Check out the open SDK + CMSIS, then run `build.sh`:

```sh
git clone https://github.com/nxp-mcuxpresso/mcux-sdk ~/mcux-sdk
git clone https://github.com/nxp-mcuxpresso/CMSIS_5 ~/CMSIS_5
MCUX_SDK=~/mcux-sdk CMSIS_DIR=~/CMSIS_5/CMSIS/Core/Include ./build.sh
```

Toolchain: `arm-none-eabi-gcc` (CPU define `CPU_MCXN947VDF_cm33_core0`,
`-mcpu=cortex-m33 -mfloat-abi=hard -mfpu=fpv5-sp-d16`, board console = LPUART4
on FRO12M via `kFRO12M_to_FLEXCOMM4`).

## Running

```sh
./run.sh                      # builds from ~/mcux-sdk if present, else skips
MCUX_ELF=/path/to/app.elf ./run.sh   # use a prebuilt ELF
```

## Model dependency uncovered by this path

The SDK gates `LPUART_Init` on `LP_FLEXCOMM_PeripheralIsPresent()`, which reads
`PSELID.UARTPRESENT` (bit 4). The model's LPUART must report the read-only
present-capability bits (UART/SPI/I2C present = `0x70`) or the SDK console
driver bails before programming BAUD/CTRL and `PRINTF` silently emits nothing.
Zephyr's driver does not check this bit — so this BSP path surfaced it.
