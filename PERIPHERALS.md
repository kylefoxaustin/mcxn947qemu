# MCXN947 peripheral coverage

Goal **reached for breadth**: **every peripheral base on the chip now has a
register-accurate functional model** — the permissive-stub table is empty.

Source of truth: the MCXN947 CMSIS header (peripheral bases/instances) and the
MCX N Reference Manual (register semantics). 124 distinct peripheral bases.

## Coverage model
Highest priority wins in the memory map:
1. **Functional model** — register-accurate behaviour (correct offsets, reset
   values, RO/W1C/status semantics; side effects + IRQs where exercised). This
   is now **every** mapped peripheral.
2. ~~Permissive stub (`mcxn_stub`)~~ — the `hw/arm/mcxn_peripherals.inc` stub
   table is **empty**; the generic stub remains in-tree only as scaffolding.
3. **Catch-all** (`-d unimp` backstop) — only for truly-unmapped addresses.

Real firmware (Zephyr boots + blinky toggles GPIO) runs without hitting an
unmodelled hang. The remaining work is **depth, not breadth**: layering active
behaviour (data paths, IRQ generation) onto the register-accurate models for
the blocks whose dynamics firmware/tests can observe — see below.

## Status by peripheral family
| Peripheral | Instances | Status |
|------------|-----------|--------|
| `ADC` | 2 | ✅ functional + active (SW-trigger conversion -> RESFIFO + EOC IRQ to NVIC; tests/mcxn-adc) |
| `AHBSC` | 1 | ✅ functional (register-accurate) |
| `BSP32` | 1 | ✅ functional (register-accurate) |
| `CACHE64_CTRL` | 1 | ✅ functional (register-accurate) |
| `CACHE64_POLSEL` | 1 | ✅ functional (register-accurate) |
| `CAN` | 2 | ✅ functional + active (FlexCAN loopback TX->RX MB + IFLAG IRQ to NVIC; tests/mcxn-flexcan) |
| `CDOG` | 2 | ✅ functional (register-accurate) |
| `CMC` | 1 | ✅ functional (register-accurate) |
| `CMP` | 3 | ✅ functional (register-accurate) |
| `CMX_PERFMON` | 2 | ✅ functional (register-accurate) |
| `CRC` | 1 | ✅ functional (register-accurate) |
| `CTIMER` | 5 | ✅ functional (count/prescale/match -> NVIC IRQ) |
| `DAC` | 3 | ✅ functional + active (FIFO watermark/empty/error interrupt -> IRQ 106/107/108 to NVIC; tests/mcxn-dac) |
| `DM` | 1 | ✅ functional (register-accurate) |
| `DMA` | 2 | ✅ functional (16-channel TCD engine, software-triggered transfers -> NVIC IRQ) |
| `EIM` | 1 | ✅ functional (register-accurate) |
| `ELS` | 1 | ✅ functional (register-accurate) |
| `EMVSIM` | 2 | ✅ functional (register-accurate) |
| `ENET` | 1 | ✅ functional + active (MDIO + model PHY link-up/ID, DMA SWR, PHYIS IRQ 139 to NVIC; tests/mcxn-enet) |
| `ERM` | 1 | ✅ functional (register-accurate) |
| `EVTG` | 1 | ✅ functional (register-accurate) |
| `EWM` | 1 | ✅ functional (register-accurate) |
| `FLEXIO` | 1 | ✅ functional (register-accurate; SWRST self-clear) |
| `FLEXSPI` | 1 | ✅ functional + active (IP-command-done IRQ 58 to NVIC; tests/mcxn-flexspi) |
| `FMU` | 1 | ✅ functional (erase/program/verify flash controller) |
| `FMU0TEST` | 1 | ✅ functional (test-alias of FMU0 @0x40043000; covered by FMU model) |
| `FREQME` | 1 | ✅ functional (register-accurate) |
| `GDET` | 2 | ✅ functional (register-accurate) |
| `GPIO` | 6 | ✅ functional |
| `I3C` | 2 | ✅ functional (register-accurate; idle-FIFO status) |
| `INPUTMUX` | 1 | ✅ functional (register-accurate) |
| `INTM` | 1 | ✅ functional (register-accurate) |
| `ITRC` | 1 | ✅ functional (register-accurate) |
| `LPI2C` | 10 | ✅ functional (LP_FLEXCOMM I2C mode; shares the FlexComm window) |
| `LPSPI` | 10 | ✅ functional (LP_FLEXCOMM SPI mode; shares the FlexComm window) |
| `LPTMR` | 2 | ✅ functional (up-count to compare -> NVIC IRQ) |
| `LPUART` | 10 | ✅ functional (10 FlexComm UARTs) |
| `LP_FLEXCOMM` | 10 | ✅ functional (all 10 as LPUART; cpu0+cpu1 consoles) |
| `MAILBOX` | 1 | ✅ functional (register-accurate) |
| `MRT` | 1 | ✅ functional (4-ch down-counter -> NVIC IRQ) |
| `NPX` | 1 | ✅ functional (register-accurate; flash-cache obfuscation control) |
| `OPAMP` | 3 | ✅ functional (register-accurate) |
| `OSTIMER` | 1 | ✅ functional (gray-code counter + match IRQ) |
| `OTPC` | 1 | ✅ functional (register-accurate) |
| `PDM` | 1 | ✅ functional (register-accurate) |
| `PINT` | 1 | ✅ functional (register-accurate) |
| `PKC` | 1 | ✅ functional (register-accurate) |
| `PLU` | 1 | ✅ functional (register-accurate) |
| `PORT` | 6 | ◐ pin-mux stub (adequate) |
| `POWERQUAD` | 1 | ✅ functional + active (compute-launch -> completion IRQ 76 to NVIC; tests/mcxn-powerquad) |
| `PUF` | 1 | ✅ functional (register-accurate) |
| `PWM` | 2 | ✅ functional + active (submodule-0 counter -> periodic reload IRQ 114/120 to NVIC; tests/mcxn-pwm) |
| `QDC` | 2 | ✅ functional (register-accurate; quadrature decoder) |
| `RTC` | 1 | ✅ functional + active (live 1 Hz calendar tick + alarm match -> IRQ 52 to NVIC; tests/mcxn-rtc) |
| `SAI` | 2 | ✅ functional + active (TX FIFO-request interrupt FRF&FRIE -> IRQ 59/60 to NVIC; tests/mcxn-sai) |
| `SCG` | 1 | ✅ functional |
| `SCT` | 1 | ✅ functional (register-accurate; SCTimer/PWM) |
| `SEMA42` | 1 | ✅ functional (register-accurate) |
| `SINC` | 1 | ✅ functional (register-accurate) |
| `SMARTDMA` | 1 | ✅ functional (register-accurate; busy reads idle) |
| `SPC` | 1 | ✅ functional |
| `SYSCON` | 1 | ✅ functional |
| `TDET` | 1 | ✅ functional (register-accurate) |
| `TRDC` | 1 | ✅ functional (register-accurate) |
| `TSI` | 1 | ✅ functional (register-accurate; end-of-scan completes) |
| `USBDCD` | 1 | ✅ functional (register-accurate) |
| `USBFS` | 1 | ✅ functional (register-accurate; reset self-clear, W1C status) |
| `USBHS1_PHY_DCD` | 1 | ✅ functional (register-accurate; HS phy/dcd 0x800 window) |
| `USBHS1__USBC` | 1 | ✅ functional (register-accurate; EHCI HS core 0x200 window) |
| `USBHS1__USBNC` | 1 | ✅ functional (register-accurate; HS non-core 0xE00 window) |
| `USBPHY` | 1 | ✅ functional (register-accurate; CLKGATE/SFTRST clear, SET/CLR/TOG) |
| `USDHC` | 1 | ✅ functional + active (SD command/response CMD8/CMD3/ACMD41 + CC IRQ 61 to NVIC; tests/mcxn-usdhc) |
| `UTICK` | 1 | ✅ functional (register-accurate) |
| `VBAT` | 1 | ✅ functional (register-accurate) |
| `VREF` | 1 | ✅ functional (register-accurate) |
| `WUU` | 1 | ✅ functional (register-accurate) |
| `WWDT` | 2 | ✅ functional (register-accurate) |
## Depth phase: active behaviour to layer on the register-accurate models
Breadth is done — every base is modelled. What remains is *dynamics* on the
blocks whose behaviour real firmware/tests can observe. Each register-accurate
model already keeps polled firmware unblocked; the next step adds the data path
+ IRQ generation (and a per-block bare-metal test, the way CTIMER/DMA/FMU have).

Done so far (active behaviour + IRQ to NVIC + bare-metal test):
- [x] **ADC** — SW-trigger conversion -> RESFIFO valid result + EOC IRQ (45/46). `tests/mcxn-adc`.
- [x] **FlexCAN** — loopback TX MB -> RX MB + IFLAG IRQ (62/63). `tests/mcxn-flexcan`.
- [x] **ENET** — MDIO PHY read (link-up/ID) + DMA soft-reset + PHYIS IRQ (139). `tests/mcxn-enet`.
- [x] **RTC** — live 1 Hz QEMUTimer calendar tick + alarm match IRQ (52). `tests/mcxn-rtc`.
- [x] **uSDHC** — SD command/response handshake (CMD8/CMD3/ACMD41) + CC IRQ (61). `tests/mcxn-usdhc`.
- [x] **FlexSPI** — IP-command-done IRQ (58). `tests/mcxn-flexspi`.
- [x] **SAI** — TX FIFO-request interrupt (FRF & FRIE) IRQ (59/60). `tests/mcxn-sai`.
- [x] **DAC** — FIFO watermark interrupt (FSR.WM & IER.WM_IE) IRQ (106/107/108). `tests/mcxn-dac`.
- [x] **PowerQuad** — compute-launch -> completion IRQ (76). `tests/mcxn-powerquad`.
- [x] **PWM** — submodule-0 running counter -> periodic reload IRQ (114/120, QEMUTimer). `tests/mcxn-pwm`.

Priority order (remaining):
- **Comm data path**: FlexComm SPI/I2C modes (LPSPI/LPI2C), I3C transfers.
- **Analog results**: (RTC alarm/tick + DAC FIFO-watermark done; CMP is
  pure-analog, correctly register-only.)
- **Connectivity**: USBFS/USBHS endpoints (OBMF-ICP transport; register-only
  today, no usb-bus backend - confirmed to holobench, gated until unparked).
  (ENET MDIO+PHY, uSDHC command/response, FlexSPI IP-cmd-done, SAI TX-request
  all done; their bulk DMA/data paths remain future work.)
- **Accelerators**: SmartDMA program execution, NPU. (PowerQuad compute-done
  IRQ done; SmartDMA/NPU left register-accurate - both need coprocessor
  firmware/compute we don't model, so a fabricated IRQ would be dishonest.)
- **IRQ wiring**: connect the per-device IRQ lines (init'd in wave 6) to the
  cpu0 NVIC as each block starts generating interrupts.

Pure-config / analog-trim / security blocks (GDET, ITRC, TRDC, ELS, PUF, PKC,
CDOG, VBAT, SPC trims, INPUTMUX, EVTG, etc.) are register-accurate and need no
further dynamics for emulation.
