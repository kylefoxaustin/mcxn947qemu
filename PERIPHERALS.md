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
| `CMP` | 3 | ✅ operator-driven (output via `comparator-output` QOM prop; edge-flag IRQ 109/110/111) |
| `CMX_PERFMON` | 2 | ✅ functional (register-accurate) |
| `CRC` | 1 | ✅ functional (register-accurate) |
| `CTIMER` | 5 | ✅ functional (count/prescale/match -> NVIC IRQ) |
| `DAC` | 3 | ✅ functional + active (FIFO watermark/empty/error interrupt -> IRQ 106/107/108 to NVIC; tests/mcxn-dac) |
| `DM` | 1 | ✅ functional (register-accurate) |
| `DMA` | 2 | ✅ functional (16-channel TCD engine, software-triggered transfers -> NVIC IRQ) |
| `EIM` | 1 | ✅ functional (register-accurate) |
| `ELS` | 1 | ✅ functional (register-accurate) |
| `EMVSIM` | 2 | ✅ functional (register-accurate) |
| `ENET` | 1 | ✅ functional + active + REAL TCP/IP (descriptor DMA TX/RX over QEMU NIC; Zephyr stack: DHCP lease + TCP echo round-trip; MAC loopback; IRQ 139; tests/mcxn-enet*) |
| `ERM` | 1 | ✅ functional (register-accurate) |
| `EVTG` | 1 | ✅ functional (register-accurate) |
| `EWM` | 1 | ✅ functional (register-accurate) |
| `FLEXIO` | 1 | ✅ functional (register-accurate; SWRST self-clear) |
| `FLEXSPI` | 1 | ✅ functional + active (IP-command-done IRQ 58 to NVIC; tests/mcxn-flexspi) + **XIP**: AHB-mapped NOR window backed by real executable memory (NS 0x8000_0000 / secure 0x9000_0000, 8 MiB) — code linked there runs in place; tests/mcxn-xip |
| `FMU` | 1 | ✅ functional (erase/program/verify flash controller) |
| `FMU0TEST` | 1 | ✅ functional (test-alias of FMU0 @0x40043000; covered by FMU model) |
| `FREQME` | 1 | ✅ functional (register-accurate) |
| `GDET` | 2 | ✅ functional (register-accurate) |
| `GPIO` | 6 | ✅ functional |
| `I3C` | 2 | ✅ functional + active (controller request -> MCTRLDONE/COMPLETE IRQ 95/96 to NVIC; tests/mcxn-i3c) |
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
| `SCT` | 1 | ✅ functional + active (running counter -> periodic match/limit event IRQ 33 to NVIC; tests/mcxn-sct) |
| `SEMA42` | 1 | ✅ functional (register-accurate) |
| `SINC` | 1 | ✅ functional (register-accurate) |
| `SMARTDMA` | 1 | ✅ functional (register-accurate; busy reads idle) |
| `SPC` | 1 | ✅ functional |
| `SYSCON` | 1 | ✅ functional |
| `TDET` | 1 | ✅ functional (register-accurate) |
| `TRDC` | 1 | ✅ functional (register-accurate) |
| `TSI` | 1 | ✅ operator-driven (per-channel count via `tsi-countN` QOM prop; end-of-scan IRQ 101) |
| `USBDCD` | 1 | ✅ functional (register-accurate) |
| `USBFS` | 1 | ✅ functional + active **device mode** (KHCI BDT endpoint engine; enumerates AND moves bulk data both directions end-to-end over usbredir to a remote USB host; IRQ 50; tests/mcxn-usb) |
| `USBHS1_PHY_DCD` | 1 | ✅ functional (register-accurate; HS phy/dcd 0x800 window) |
| `USBHS1__USBC` | 1 | ✅ functional + active **device mode** (ChipIdea dQH/dTD endpoint engine; enumerates + bulk data both ways at high-speed over usbredir; IRQ 67; tests/mcxn-usb-hs) |
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
- [x] **FlexSPI XIP** — AHB-mapped NOR window is real executable memory (NS 0x8000_0000 / secure alias 0x9000_0000, 8 MiB W25Q64 per the N947 DTS). Code linked into the window runs in place (no copy to SRAM); the `-kernel` loader fills it. `tests/mcxn-xip` boots from internal flash and calls a routine executing at 0x9000_0000.
- [x] **SAI** — TX FIFO-request interrupt (FRF & FRIE) IRQ (59/60). `tests/mcxn-sai`.
- [x] **DAC** — FIFO watermark interrupt (FSR.WM & IER.WM_IE) IRQ (106/107/108). `tests/mcxn-dac`.
- [x] **PowerQuad** — compute-launch -> completion IRQ (76). `tests/mcxn-powerquad`.
- [x] **PWM** — submodule-0 running counter -> periodic reload IRQ (114/120, QEMUTimer). `tests/mcxn-pwm`.
- [x] **SCT** — running counter -> periodic match/limit event-0 IRQ (33, QEMUTimer). `tests/mcxn-sct`.
- [x] **I3C** — controller request -> transfer-complete IRQ (95/96). `tests/mcxn-i3c`.
- [x] **ENET MAC frame path** — DWC ENET-QoS descriptor-ring TX/RX over a real QEMU NIC backend (-nic) + MAC loopback, TI/RI DMA IRQ. `tests/mcxn-enet-mac`. **Cross-board ready.**
- [x] **FlexComm SPI/I2C** — LP_FLEXCOMM PSELID function-select: LPSPI master loopback (TDR->RDR, WCF/FCF/TCF + RDF) and LPI2C controller (START/TX/RX/STOP echo target, SDF/EPF), both raising the shared FlexComm NVIC line via ISTAT. `tests/mcxn-flexcomm` (LPSPI on FC3 IRQ 38, LPI2C on FC0 IRQ 35).
- [x] **EMVSIM0/1** — software-driven TX: TX_BUF write -> synchronous transmit -> TX_STATUS.TCF/ETCF/TDTF/TFE latched -> transmit-complete IRQ when the matching INT_MASK enable bit is clear (RM: 0=enabled). `tests/mcxn-emvsim` (IRQ 103/104). Moved out of cfgdev into explicit NVIC-wired instantiation.
- [x] **MAILBOX (inter-CPU)** — cross-core interrupt: each CPU's IRQ word (MBOXIRQ[n].IRQ, set via IRQSET/cleared via IRQCLR) drives MAILBOX_IRQn=54 on THAT core's NVIC (IRQ[0]->cpu0, IRQ[1]->cpu1). cpu0<->cpu1 ping-pong proven. `tests/mcxn-mailbox`. The OpenAMP/rpmsg notification path; moved out of cfgdev into explicit two-line instantiation.

Priority order (remaining):
- **Comm data path**: (FlexComm LPSPI/LPI2C done; I3C transfer-complete done;
  EMVSIM TX-complete done; PDM/SINC are input-driven — register-only-honest,
  no software-observable interrupt source without mic/modulator stimulus.)
- **Analog results**: (RTC alarm/tick + DAC FIFO-watermark done; ADC/CMP/TSI
  are operator-driven — analog inputs exposed as QOM properties so an operator
  injects what a board pin would drive, with conversion/edge/scan IRQs wired.)
- **Connectivity**: USBFS device-mode endpoints now ENUMERATE over usbredir
  (KHCI BDT engine -> shared usbredir-server core -> remote USB host; the
  OBMF-ICP / inter-QEMU transport, mission #5). `tests/mcxn-usb` proves
  enumeration against a self-contained usbredir host; the i.MX93-host link is
  the next integration step. USBHS (ChipIdea) device mode is the follow-on
  backend on the same core.
  (ENET now has full MAC frame DMA-ring + QEMU NIC - cross-board ready;
  uSDHC command/response, FlexSPI IP-cmd-done, SAI TX-request done.)
- **Accelerators**: SmartDMA program execution, NPU. (PowerQuad compute-done
  IRQ done; SmartDMA/NPU left register-accurate - both need coprocessor
  firmware/compute we don't model, so a fabricated IRQ would be dishonest.)
- **IRQ wiring**: connect the per-device IRQ lines (init'd in wave 6) to the
  cpu0 NVIC as each block starts generating interrupts.

Pure-config / analog-trim / security blocks (GDET, ITRC, TRDC, ELS, PUF, PKC,
CDOG, VBAT, SPC trims, INPUTMUX, EVTG, etc.) are register-accurate and need no
further dynamics for emulation.
