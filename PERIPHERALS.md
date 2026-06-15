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
| `CDOG` | 2 | ○ stub (present, readback, non-blocking) |
| `CMC` | 1 | ○ stub (present, readback, non-blocking) |
| `CMP` | 3 | ○ stub (present, readback, non-blocking) |
| `CMX_PERFMON` | 2 | ○ stub (present, readback, non-blocking) |
| `CRC` | 1 | ○ stub (present, readback, non-blocking) |
| `CTIMER` | 5 | ○ stub (present, readback, non-blocking) |
| `DAC` | 3 | ○ stub (present, readback, non-blocking) |
| `DM` | 1 | ○ stub (present, readback, non-blocking) |
| `DMA` | 2 | ○ stub (present, readback, non-blocking) |
| `EIM` | 1 | ○ stub (present, readback, non-blocking) |
| `ELS` | 1 | ○ stub (present, readback, non-blocking) |
| `EMVSIM` | 2 | ○ stub (present, readback, non-blocking) |
| `ENET` | 1 | ○ stub (present, readback, non-blocking) |
| `ERM` | 1 | ○ stub (present, readback, non-blocking) |
| `EVTG` | 1 | ○ stub (present, readback, non-blocking) |
| `EWM` | 1 | ○ stub (present, readback, non-blocking) |
| `FLEXIO` | 1 | ○ stub (present, readback, non-blocking) |
| `FLEXSPI` | 1 | ○ stub (present, readback, non-blocking) |
| `FMU` | 1 | ○ stub (present, readback, non-blocking) |
| `FMU0TEST` | 1 | ○ stub (present, readback, non-blocking) |
| `FREQME` | 1 | ○ stub (present, readback, non-blocking) |
| `GDET` | 2 | ○ stub (present, readback, non-blocking) |
| `GPIO` | 6 | ✅ functional |
| `I3C` | 2 | ○ stub (present, readback, non-blocking) |
| `INPUTMUX` | 1 | ○ stub (present, readback, non-blocking) |
| `INTM` | 1 | ○ stub (present, readback, non-blocking) |
| `ITRC` | 1 | ○ stub (present, readback, non-blocking) |
| `LPI2C` | 10 | ○ stub (present, readback, non-blocking) |
| `LPSPI` | 10 | ○ stub (present, readback, non-blocking) |
| `LPTMR` | 2 | ○ stub (present, readback, non-blocking) |
| `LPUART` | 10 | ◐ console only (FlexComm4); others stubbed |
| `LP_FLEXCOMM` | 10 | ◐ console only (FlexComm4); others stubbed |
| `MAILBOX` | 1 | ○ stub (present, readback, non-blocking) |
| `MRT` | 1 | ○ stub (present, readback, non-blocking) |
| `NPX` | 1 | ○ stub (present, readback, non-blocking) |
| `OPAMP` | 3 | ○ stub (present, readback, non-blocking) |
| `OSTIMER` | 1 | ○ stub (present, readback, non-blocking) |
| `OTPC` | 1 | ○ stub (present, readback, non-blocking) |
| `PDM` | 1 | ○ stub (present, readback, non-blocking) |
| `PINT` | 1 | ○ stub (present, readback, non-blocking) |
| `PKC` | 1 | ○ stub (present, readback, non-blocking) |
| `PLU` | 1 | ○ stub (present, readback, non-blocking) |
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
| `TDET` | 1 | ○ stub (present, readback, non-blocking) |
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
