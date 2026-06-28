# NXP MCX N QEMU machine model (scaffold)

A part-agnostic QEMU machine model for NXP's MCX N-series (Arm Cortex-M33)
microcontrollers, kept self-contained (all logic in new `mcxn_*` files, no
edits to generic QEMU). The first
target board is `frdm-mcxn947`, but the SoC is driven by a per-SKU config table
so other MCX N (and, with a different `cpu_type`/core count, MCX A/W) parts drop
in as additional table entries.

## Status

This is a **boot scaffold**, not a finished model. What's here:

- `MCXN_SOC` container: Cortex-M33 + NVIC + SysTick via the `ARMV7M` object,
  code-flash + SRAM regions, and a single catch-all `unimplemented` region over
  the whole peripheral window (both secure + non-secure aliases).
- `frdm-mcxn947` board: source clocks, SoC instantiation, firmware load.
- Config-table architecture so adding a SKU = adding a struct.

The MCXN947 config is now populated with **CMSIS-verified values** (see below).

What's intentionally **not** here yet:

- Most peripheral models (GPIO/PORT, timers, FlexCAN, NPU...).
- cpu1 (second M33). MVP runs cpu0 only — matching the Zephyr port, which also
  only runs cpu0 standalone by default.

Modelled so far: Cortex-M33 core, flash/SRAM, FlexComm4/LPUART4 console (TX/RX/
IRQ), SCG0 clock-generator stub, and a catch-all over the rest of the peripheral
window. Console and SCG0 are mapped at both the non-secure (`0x400x`) and secure
(`0x500x`) TrustZone-M aliases.

## MCXN947 — verified facts (CMSIS `MCXN947_cm33_core0.h`)

| Property            | Value                                              |
|---------------------|----------------------------------------------------|
| Core                | dual Arm Cortex-M33 @ up to 150 MHz (cpu0 in MVP)  |
| Core features       | FPU, DSP, MPU, SAU / TrustZone-M                    |
| NVIC external IRQs  | **156** (highest `CTI0_IRQn` = 155)                |
| `__NVIC_PRIO_BITS`  | **3**                                              |
| Flash               | 2 MiB @ `0x0000_0000`                              |
| SRAM                | 512 KiB @ `0x2000_0000` (banked RAMA..H, contig.)  |
| Console UART        | FlexComm4 / LPUART4 @ `0x400B_4000` (NS)            |
| FlexCAN             | CAN0 @ `0x400D_4000`, CAN1 @ `0x400D_8000`         |
| eIQ Neutron NPU     | IRQ 97; base from RM (absent from CMSIS header)     |

**TrustZone-M aliasing:** every peripheral is mapped twice — non-secure at
`0x400x_xxxx` and secure at `0x500x_xxxx`. The catch-all spans
`0x4000_0000..0x5FFF_FFFF` to cover both.

## Bring-up methodology

The catch-all peripheral stub is the whole point. Run real firmware with:

```
-d unimp,guest_errors
```

and every MMIO access into unmodelled space is logged with its address. That
log *is* your prioritised peripheral to-do list — you implement the device the
firmware actually touches next, in the order it touches it, instead of guessing
from the RM. Replace a slice of the catch-all with a real `SysBusDevice`, repeat.

Recommended first milestone: get a banner out over **semihosting**
(`-semihosting-config enable=on,target=native`) so you have console before
writing any UART model. The first *real* peripheral to model is then FlexComm0
in USART mode, which gives you a hardware-accurate console.

## Memory map (MCXN947 — CMSIS-verified)

| Region            | Base          | Size        | Notes                         |
|-------------------|---------------|-------------|-------------------------------|
| Code flash        | `0x0000_0000` | 2 MiB       | RAM-backed for bring-up       |
| SRAM              | `0x2000_0000` | 512 KiB     | RAM (banked, mapped contig.)  |
| Peripherals (NS)  | `0x4000_0000` | —           | catch-all `unimplemented`     |
| Peripherals (S)   | `0x5000_0000` | —           | secure alias, same catch-all  |
| FlexSPI NOR (NS)  | `0x8000_0000` | 8 MiB       | AHB-mapped ext flash (XIP)    |
| FlexSPI NOR (S)   | `0x9000_0000` | 8 MiB       | secure alias of the XIP window|
| PPB (NVIC/SysTick)| `0xE000_0000` | —           | handled by `ARMV7M` container |

## Next step: FlexComm4 / LPUART4 console

The first real peripheral to model. The win here is reuse: MCX N's console runs
the **standard NXP LPUART** register block (`VERID/PARAM/BAUD/STAT/CTRL/DATA/
FIFO/WATER`, base offsets `0x0/0x4/0x10/0x14/0x18/0x1C/0x28/0x2C`) — the *same
IP* as the i.MX 93/95 LPUART. An existing i.MX LPUART model should port over
almost unchanged. The only MCX-specific wrinkle is the `LP_FLEXCOMM` wrapper at
the same base, which function-selects USART vs SPI vs I2C; for console you model
the LPUART registers at `0x400B_4000` and stub the function-select to "USART".

Until then, `-semihosting-config enable=on,target=native` gives console output
without any UART model.

## Build & run

```sh
# from the QEMU tree, after applying QEMU-INTEGRATION.md
./configure --target-list=arm-softmmu
make -j"$(nproc)"

./build/qemu-system-arm \
    -M frdm-mcxn947 \
    -kernel firmware.elf \
    -nographic \
    -semihosting-config enable=on,target=native \
    -d unimp,guest_errors
```

## Files

| File                          | Role                                            |
|-------------------------------|-------------------------------------------------|
| `include/hw/arm/mcxn_soc.h`   | SoC type + `MCXNConfig` per-SKU struct          |
| `hw/arm/mcxn_soc.c`           | config table, memories, M33 core, periph stub   |
| `hw/arm/mcxn_frdm.c`          | `frdm-mcxn947` board                            |
| `QEMU-INTEGRATION.md`         | Kconfig + meson.build edits, file placement     |

## Version-sensitive touchpoints

Written against current QEMU mainline. If building against an older tree, check:

- `ARMV7M` clock inputs are named `cpuclk` / `refclk` (confirmed on master).
- `armv7m_load_kernel()` takes `(cpu, filename, mem_base, mem_size)` — the
  `mem_base` argument was added relatively recently.
- `device_class_set_props()` / `DEFINE_TYPES()` are the current idioms.
