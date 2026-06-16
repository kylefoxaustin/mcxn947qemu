# MCXN947 peripheral coverage

Tracking toward the goal: **every peripheral on the chip functional**.

Source of truth: the MCXN947 CMSIS header (peripheral bases/instances) and the
MCX N Reference Manual (register semantics). 124 distinct peripheral bases.

## Coverage model
Three levels, highest priority wins in the memory map:
1. **Functional model** — real behaviour (registers + side effects + IRQs).
2. **Permissive stub** (`mcxn_stub`) — present, register read-back, never
   blocks. Adequate for pure-config/analog/security blocks; a placeholder for
   active blocks until they get a real model. Generated from
   `hw/arm/mcxn_peripherals.inc`.
3. **Catch-all** (`-d unimp` backstop) — only for truly-unmapped addresses.

Every peripheral is at least level 2, so real firmware (Zephyr boots +
blinky toggles GPIO) runs without hitting an unmodelled hang.

## Status by peripheral family
| Peripheral | Instances | Status |
|------------|-----------|--------|
| `ADC` | 2 | ○ stub (present, readback, non-blocking) |
| `AHBSC` | 1 | ○ stub (present, readback, non-blocking) |
| `BSP32` | 1 | ○ stub (present, readback, non-blocking) |
| `CACHE64_CTRL` | 1 | ○ stub (present, readback, non-blocking) |
| `CACHE64_POLSEL` | 1 | ○ stub (present, readback, non-blocking) |
| `CAN` | 2 | ○ stub (present, readback, non-blocking) |
| `CDOG` | 2 | ✅ functional (register-accurate) |
| `CMC` | 1 | ✅ functional (register-accurate) |
| `CMP` | 3 | ○ stub (present, readback, non-blocking) |
| `CMX_PERFMON` | 2 | ○ stub (present, readback, non-blocking) |
| `CRC` | 1 | ✅ functional (register-accurate) |
| `CTIMER` | 5 | ✅ functional (count/prescale/match -> NVIC IRQ) |
| `DAC` | 3 | ○ stub (present, readback, non-blocking) |
| `DM` | 1 | ○ stub (present, readback, non-blocking) |
| `DMA` | 2 | ○ stub (present, readback, non-blocking) |
| `EIM` | 1 | ✅ functional (register-accurate) |
| `ELS` | 1 | ○ stub (present, readback, non-blocking) |
| `EMVSIM` | 2 | ○ stub (present, readback, non-blocking) |
| `ENET` | 1 | ○ stub (present, readback, non-blocking) |
| `ERM` | 1 | ✅ functional (register-accurate) |
| `EVTG` | 1 | ✅ functional (register-accurate) |
| `EWM` | 1 | ✅ functional (register-accurate) |
| `FLEXIO` | 1 | ○ stub (present, readback, non-blocking) |
| `FLEXSPI` | 1 | ○ stub (present, readback, non-blocking) |
| `FMU` | 1 | ✅ functional (erase/program/verify flash controller) |
| `FMU0TEST` | 1 | ○ stub (present, readback, non-blocking) |
| `FREQME` | 1 | ✅ functional (register-accurate) |
| `GDET` | 2 | ✅ functional (register-accurate) |
| `GPIO` | 6 | ✅ functional |
| `I3C` | 2 | ○ stub (present, readback, non-blocking) |
| `INPUTMUX` | 1 | ✅ functional (register-accurate) |
| `INTM` | 1 | ✅ functional (register-accurate) |
| `ITRC` | 1 | ✅ functional (register-accurate) |
| `LPI2C` | 10 | ○ stub (present, readback, non-blocking) |
| `LPSPI` | 10 | ○ stub (present, readback, non-blocking) |
| `LPTMR` | 2 | ✅ functional (up-count to compare -> NVIC IRQ) |
| `LPUART` | 10 | ✅ functional (10 FlexComm UARTs) |
| `LP_FLEXCOMM` | 10 | ✅ functional (all 10 as LPUART; cpu0+cpu1 consoles) |
| `MAILBOX` | 1 | ○ stub (present, readback, non-blocking) |
| `MRT` | 1 | ✅ functional (4-ch down-counter -> NVIC IRQ) |
| `NPX` | 1 | ○ stub (present, readback, non-blocking) |
| `OPAMP` | 3 | ○ stub (present, readback, non-blocking) |
| `OSTIMER` | 1 | ○ stub (present, readback, non-blocking) |
| `OTPC` | 1 | ○ stub (present, readback, non-blocking) |
| `PDM` | 1 | ○ stub (present, readback, non-blocking) |
| `PINT` | 1 | ○ stub (present, readback, non-blocking) |
| `PKC` | 1 | ○ stub (present, readback, non-blocking) |
| `PLU` | 1 | ✅ functional (register-accurate) |
| `PORT` | 6 | ◐ pin-mux stub (adequate) |
| `POWERQUAD` | 1 | ○ stub (present, readback, non-blocking) |
| `PUF` | 1 | ○ stub (present, readback, non-blocking) |
| `PWM` | 2 | ○ stub (present, readback, non-blocking) |
| `QDC` | 2 | ○ stub (present, readback, non-blocking) |
| `RTC` | 1 | ○ stub (present, readback, non-blocking) |
| `SAI` | 2 | ○ stub (present, readback, non-blocking) |
| `SCG` | 1 | ✅ functional |
| `SCT` | 1 | ○ stub (present, readback, non-blocking) |
| `SEMA42` | 1 | ○ stub (present, readback, non-blocking) |
| `SINC` | 1 | ○ stub (present, readback, non-blocking) |
| `SMARTDMA` | 1 | ○ stub (present, readback, non-blocking) |
| `SPC` | 1 | ✅ functional |
| `SYSCON` | 1 | ✅ functional |
| `TDET` | 1 | ✅ functional (register-accurate) |
| `TRDC` | 1 | ○ stub (present, readback, non-blocking) |
| `TSI` | 1 | ○ stub (present, readback, non-blocking) |
| `USBDCD` | 1 | ○ stub (present, readback, non-blocking) |
| `USBFS` | 1 | ○ stub (present, readback, non-blocking) |
| `USBHS1_PHY_DCD` | 1 | ○ stub (present, readback, non-blocking) |
| `USBHS1__USBC` | 1 | ○ stub (present, readback, non-blocking) |
| `USBHS1__USBNC` | 1 | ○ stub (present, readback, non-blocking) |
| `USBPHY` | 1 | ○ stub (present, readback, non-blocking) |
| `USDHC` | 1 | ○ stub (present, readback, non-blocking) |
| `UTICK` | 1 | ○ stub (present, readback, non-blocking) |
| `VBAT` | 1 | ○ stub (present, readback, non-blocking) |
| `VREF` | 1 | ○ stub (present, readback, non-blocking) |
| `WUU` | 1 | ○ stub (present, readback, non-blocking) |
| `WWDT` | 2 | ○ stub (present, readback, non-blocking) |
## Next functional upgrades (active blocks that need real behaviour)
Priority order — blocks whose behaviour real firmware/tests can observe:
- **Timers/counters** → interrupts: CTIMER (x5), MRT, OSTIMER, LPTMR (x2), RTC, SCT.
- **Comm** → data path: the FlexComm UART/SPI/I2C (LPUART other than 4, LPI2C, LPSPI), I3C.
- **DMA** (x2) → memory transfers + completion IRQs.
- **Analog** → results: ADC (x2), DAC (x3), CMP (x3), OPAMP (x3).
- **Connectivity**: ENET, USBFS/USBHS, FlexCAN (x2), SAI (x2), FLEXSPI, SDHC.
- **Accelerators**: eIQ Neutron NPU, SmartDMA, PowerQuad, CRC.

Pure-config / analog-trim / security blocks (GDET, ITRC, TRDC, ELS, PUF, CDOG,
VBAT, SPC voltage trims, INPUTMUX, EVTG, etc.) are functionally complete as
permissive stubs for emulation purposes.
