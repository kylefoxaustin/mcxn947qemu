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
| `NPX` | 1 | ✅ functional (register-accurate; flash-cache obfuscation control @ 0x400C_C000) |
| `Neutron NPU` | 1 | ✅ FLAG-AT-OPERATOR (eIQ Neutron N1-16 @ 0x400B_E000, IRQ 97; proprietary microcode compute — CTRL handshake acked so no hang, result honestly uncomputed: compute-modelled=false + jobs-started; tests/mcxn-neutron) |
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

### eDMA byte-access audit (fleet lesson: silent-drop of narrow DMA bursts)
A peripheral driven by BOTH the CPU (32-bit) and eDMA (byte/halfword bursts to a
FIFO data register) MUST accept sub-word MMIO, or the memory core silently drops
the DMA bytes (transfer "completes", no data moves) — a top-class silent-wrong
bug (origin: 95emulator's LPSPI-over-eDMA case).
- ✅ **FlexComm (LPUART/LPSPI/LPI2C)** — fixed (`.valid/.impl min_access_size=1`);
  proven a 1-byte write to LPSPI TDR now transfers. Handlers dispatch to specific
  registers, no corrupting generic default.
- ✅ **SAI, ADC** — already accept byte access.
- ⏳ **DAC, PDM, SINC** — still 4-byte-only AND use a generic
  `default: regs[offset/4] = value`, which would *corrupt* a config register on
  an aligned sub-word write. Before relaxing to byte access, give the default a
  sub-word read-modify-merge + add a byte/halfword-DMA test per block.

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
- [x] **Dual-core rpmsg substrate (end-to-end)** — real inter-core message DATA carried through shared-SRAM ring buffers (a simplified vring: per-ring producer/consumer indices + payload slots) with the MAILBOX as the doorbell — the OpenAMP/rpmsg transport pattern. cpu0 pushes payloads to a TX ring + kicks cpu1; cpu1 drains, transforms each payload, publishes to an RX ring + kicks cpu0; cpu0 verifies every payload round-tripped. 20 messages over 4 doorbell batches, byte-exact. `tests/mcxn-rpmsg`. Proves the model provides everything OpenAMP needs (dual M33 + shared SRAM + cross-core mailbox IRQ) — the last north-star item (rpmsg) validated with real data, not just a notification.
- [x] **Board-to-board (inter-QEMU) transport nodes** — the MCX is a drop-in b2b node on the fleet's link fabric (holobench labs / the i.MX91-93 interconnect), all over a chardev socket, byte-exact:
  - **UART** (`tests/mcxn-uart-link`) — FlexComm2/LPUART2 = serial_hd(1) on a socket; raw byte stream, no protocol. Surfaced + fixed a real streaming-RX bug (LPUART RX now calls `qemu_chr_fe_accept_input`, else RX stalled after one byte under flow control).
  - **SPI** (`tests/mcxn-spi-link`) — FlexComm5 LPSPI as an SSI-bus master over the fleet `spi-link` peripheral (`hw/ssi/spi_link.c`, ported from 91); `-device spi-link,bus=mcxn-lpspi,chardev=<sock>`. LPSPI-mode TDR shifts over the bus (ssi_transfer) when a bus-name is set, internal loopback otherwise. Both directions byte-exact (91↔MCX cross-tree cross-check with real Linux fsl-lpspi passed).
  - **CAN** (`tests/mcxn-can-link`) — FlexCAN0 as a CAN-bus client over the fleet `can-host-chardev` backend (`net/can/can_host_chardev.c`, ported from 95). Wired the **fleet-standard way**, identical to i.MX 91/93/95: `-object can-bus,id=cb -machine canbus0=cb -object can-host-chardev,canbus=cb,chardev=<sock>` (the frdm board is a custom machine type exposing `canbus0/canbus1` link props; it also accepts a bus simply *named* `canbus0/1` as a fallback). A TX message buffer builds a `qemu_can_frame` onto the bus; bus frames land in RX MBs. Both directions byte-exact; internal loopback preserved when no bus.
  - Plus USB (bulk + CDC-serial) + ENET already done — so the MCX pairs with imx91/93 over **all five transports: ENET/UART/USB/SPI/CAN**.

Priority order (remaining):
- **Comm data path**: (FlexComm LPSPI/LPI2C done; I3C transfer-complete done;
  EMVSIM TX-complete done; PDM/SINC are input-driven — register-only-honest,
  no software-observable interrupt source without mic/modulator stimulus.)
- **Analog results**: (RTC alarm/tick + DAC FIFO-watermark done; ADC/CMP/TSI
  are operator-driven — analog inputs exposed as QOM properties so an operator
  injects what a board pin would drive, with conversion/edge/scan IRQs wired.)
- **Connectivity**: BOTH USB controllers do full device-mode data path over
  usbredir (the OBMF-ICP / inter-QEMU transport, mission #5): USBFS0 (full-speed,
  KHCI BDT engine; `tests/mcxn-usb`) and USBHS1 (high-speed, ChipIdea dQH/dTD
  engine; `tests/mcxn-usb-hs`), each on its own usbredir core/socket. Enumeration
  (incl. Linux-style partial+full config reads + GET_STATUS) + bulk echo proven
  against a self-contained host. **✅ M4 done: the live i.MX93↔MCX USB link is
  verified end-to-end** — a real i.MX93 stock-BSP Linux kernel enumerates and
  configures the MCX coherent HS gadget at high-speed (480 Mb/s) over a unix
  socket (`tests/mcxn-usb-link/`; MCX = server, i.MX93 = stock `-device
  usb-redir`). Getting there peeled off four real-kernel-only bugs on the MCX
  side (interface_info/ep_info at connect, server persistence, the dedicated
  set_configuration message, a coherent HS descriptor set + device_qualifier)
  and one on the i.MX93 side (ChipIdea PORTSC.PSPD). **✅ CDC-ACM serial link
  DONE — proven on TWO hosts (i.MX93 + i.MX91):** the MCX also presents a real
  USB CDC-ACM device (`tests/mcxn-usb-cdc`, `gadget-profile=cdc`); both stock-BSP
  Linux kernels bind `cdc_acm` → `/dev/ttyACM0` and round-trip bytes byte-exact
  over the live link. Needed the CDC control path (SET_LINE_CODING control-OUT
  data stage; bulk-OUT `actual_length`; EP1-OUT armed before the SET_CONFIG
  status stage) and the usbredir interrupt-receiving/cancel callbacks (a real
  importer NULL-crashes the gadget on the CDC notification EP otherwise). Known
  host-maskable quirk: a first-write-after-bind timing race (host settles ~50 ms
  or retries on EIO).
  (ENET now has full MAC frame DMA-ring + QEMU NIC - cross-board ready;
  uSDHC command/response, FlexSPI IP-cmd-done, SAI TX-request done.)
- **Accelerators**: PowerQuad compute-done IRQ done (+ CP0 transcendentals).
  Neutron NPU now FLAG-AT-OPERATOR @ 0x400B_E000 (proprietary microcode: the
  CTRL exec/done handshake is acked so eIQ inference does not hang, but the
  result is honestly flagged uncomputed via QMP, never silently fabricated;
  operator opt-in error-trap to the guest). SmartDMA left register-accurate
  (also FLAG-AT-OPERATOR for program output) - the EZH coprocessor firmware
  isn't modelled, so a fabricated result would be dishonest.
- **IRQ wiring**: connect the per-device IRQ lines (init'd in wave 6) to the
  cpu0 NVIC as each block starts generating interrupts.

Pure-config / analog-trim / security blocks (GDET, ITRC, TRDC, ELS, PUF, PKC,
CDOG, VBAT, SPC trims, INPUTMUX, EVTG, etc.) are register-accurate and need no
further dynamics for emulation.
