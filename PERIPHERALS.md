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
| `CAN` | 2 | ✅ **real RX matching + b2b** — the board-to-board path did NO ID MATCHING (a driver trusting its own filter read another node's frame) and SILENTLY DROPPED a frame when no mailbox was free; per the RM a frame landing on an unserviced MB now OVERWRITES it and sets CODE=OVERRUN, so the data moves AND the guest is told. A disabled module (MCR[MDIS]) no longer receives OR transmits. Live board-to-board link; tests/mcxn-flexcan, mcxn-flexcan-rx, mcxn-can-link |
| `CDOG` | 2 | ✅ functional (register-accurate) |
| `CMC` | 1 | ✅ functional (register-accurate) |
| `CMP` | 3 | ✅ operator-driven (output via `comparator-output` QOM prop; edge-flag IRQ 109/110/111) + **eDMA request** (CCR1[DMA_EN] redirects an IER-enabled edge to HsCmp{n}=28+n as a one-shot pulse), operator injects the crossing over QMP, mutation-proven on data + rate; tests/mcxn-cmp-dma |
| `CMX_PERFMON` | 2 | ✅ functional (register-accurate) |
| `CRC` | 1 | ✅ functional (register-accurate) |
| `CTIMER` | 5 | ✅ functional (count/prescale/match -> NVIC IRQ) + **match-paced eDMA** (M0=7+2k/M1=8+2k, one-shot pulse per match), mutation-proven on data + rate axes; tests/mcxn-ctimer-dma. **Input capture** (ch0): an operator-driven `capture-input` edge (CCR[CAP0RE]/[CAP0FE]) latches the live counter into CR0 + raises IR[CR0INT] (CCR[CAP0I]) — timestamp/pulse-width, was dead storage; mutation-proven on capture + edge-select; tests/mcxn-ctimer-capture. Also drives ADCn_TRIG (match-3, mcxn-adc-ctimer-trig) |
| `DAC` | 3 | ✅ functional + active (FIFO watermark/empty/error interrupt -> IRQ 106/107/108 to NVIC; tests/mcxn-dac) |
| `DM` | 1 | ✅ functional (register-accurate) |
| `DMA` | 2 | ✅ **software AND peripheral-triggered** — 16-channel TCD engine; CH_CSR[ERQ] used to be a DEAD BIT so DMA-driven audio/ADC/UART could not run at all. Peripherals now drive one request line per CMSIS mux source, one MINOR LOOP per request, serviced in a bottom half (servicing inline is a re-entrant MMIO access QEMU SILENTLY DROPS). ⚠ Wired for SAI + DAC only — ADC/PDM/SINC/LPFlexcomm still have none; tests/mcxn-dma, mcxn-sai-dma, mcxn-dac-dma |
| `EIM` | 1 | ✅ functional (register-accurate) |
| `ELS` | 1 | ⚠️ **HONEST-FAULT** — was the worst bug in the tree: every crypto command DMA'd its result to a buffer that was NEVER WRITTEN, so firmware read UNINITIALISED MEMORY as its signature/digest/session key and an ECDSA verify would "succeed" on garbage. Now every unmodelled crypto op FAULTS to the guest (`ELS_STATUS[ELS_ERR]` + `ELS_ERR_STATUS[OPN_ERR]`), result buffer untouched, BUSY still clears so no hang. RND_REQ is real (PRNG DMA'd); tests/mcxn-els |
| `EMVSIM` | 2 | ⚠️ **RETRACTED to registers/IRQs only** — NO DATA PATH. A smartcard interface needs a card and there is no card model upstream (unlike sd-card/m25p80/at24c). A stated gap is honest; a badge over one is a bug |
| `ENET` | 1 | ✅ functional + active + REAL TCP/IP (descriptor DMA TX/RX over QEMU NIC; Zephyr stack: DHCP lease + TCP echo round-trip; MAC loopback; IRQ 139; tests/mcxn-enet*) |
| `ERM` | 1 | ✅ functional (register-accurate) |
| `EVTG` | 1 | ✅ functional (register-accurate) |
| `EWM` | 1 | ✅ functional (register-accurate) |
| `FLEXIO` | 1 | ✅ functional (register-accurate; SWRST self-clear) |
| `FLEXSPI` | 1 | ✅ functional + active (IP-command-done IRQ 58 to NVIC; tests/mcxn-flexspi) + **XIP**: AHB-mapped NOR window backed by real executable memory (NS 0x8000_0000 / secure 0x9000_0000, 8 MiB) — code linked there runs in place; tests/mcxn-xip + **QSPI execute-in-place boot** (`-machine frdm-mcxn947,qspi-boot=on`): the M33 resets from the NOR (secure alias 0x9000_0000), no internal-flash image; a valid FlexSPI Config Block (tag "FCFB" at offset 0x400) is required or the image is rejected loud+fatal; tests/mcxn-qspi-boot (mutation-proven on the FCB check AND the reset reroute) + **eDMA request lines** (RX=1/TX=2, gated by IP{RX,TX}FCR[DMAEN]) so FLEXSPI_TransferEDMA works; tests/mcxn-flexspi-dma |
| `FMU` | 1 | ✅ functional (erase/program/verify flash controller) |
| `FMU0TEST` | 1 | ✅ functional (test-alias of FMU0 @0x40043000; covered by FMU model) |
| `FREQME` | 1 | ✅ functional (register-accurate) |
| `GDET` | 2 | ✅ functional (register-accurate) |
| `GPIO` | 6 | ✅ functional |
| `I3C` | 2 | ✅ functional + active (controller request -> MCTRLDONE/COMPLETE IRQ 95/96 to NVIC; tests/mcxn-i3c) |
| `INPUTMUX` | 1 | ✅ functional — register-accurate + real trigger routing: an LPTMR compare (selector 50), a CTIMER0/1/2 match-3 (selectors 5/6/7), or a FlexPWM0/1 SM0 output trigger (selectors 24/25, 32/33) routes to ADCn_TRIG → the ADC's HTEN-gated hardware trigger, so "convert on a timer tick/match/PWM-compare" works (the motor-control synchronous-sampling path). Selectors are PER-DESTINATION where silicon says so: selector 8/9 = CTIMER3/4 M3 for ADC0 but CTIMER3 M2 / CTIMER4 M1 for ADC1 (mutation-proven). The same router drives DACn_TRIG → DAC output-FIFO advance (timer-paced waveform, GCR[TRGSEL]-gated), CMPn_TRIG → comparator round-robin sampling (RRF flags a deviation, RRCR0[RR_TRG_SEL]-gated), QDCn_TRIG → quadrature-decoder position capture/clear (CTRL2[UPDHLD]/[UPDPOS]), and TSI_TRIG → touch-sensing scan (GENCS[STM]-gated). Also gates every eDMA request line (DMAn_REQ_ENABLE). tests/mcxn-adc-hwtrig, mcxn-adc-ctimer-trig, mcxn-adc-ctimer-adc1-trig, mcxn-adc-pwm-trig, mcxn-dac-hwtrig, mcxn-cmp-trig, mcxn-qdc-trig, mcxn-tsi-trig, mcxn-inputmux-gate |
| `INTM` | 1 | ✅ functional (register-accurate) |
| `ITRC` | 1 | ✅ functional (register-accurate) |
| `LPI2C` | 10 | ✅ functional **master → a REAL device**: drives a genuine QEMU I2C bus (`<flexcommN>-i2c`); the MTDR command engine does real START/addr/TX/RX/STOP transactions, NACK→MSR[NDF]. A real at24c EEPROM write-then-read is byte-exact (replaced a fabricated echo target); mutation-proven; tests/mcxn-lpi2c-eeprom. Slave engine not modelled |
| `LPSPI` | 10 | ✅ functional **master → a REAL m25p80 NOR**: FlexComm1 drives an on-board w25q64 (CS held across a burst via TCR[CONT]); JEDEC-ID read (0xEF4017) + WREN/page-program/read-back byte-exact against the flash (replaced a loopback echo); mutation-proven; tests/mcxn-lpspi-nor. Loopback (no device) + b2b spi-link still supported |
| `LPTMR` | 2 | ✅ functional (up-count to compare -> NVIC IRQ) — clock **selected by PSR[PCS]** per RM Table 463 (00 FRO_12M / 01 FRO_16K / 10 32K_CLK / 11 OSC_SYS-seam), a low-power clock not the 150 MHz bus clock it wrongly used before; tests/mcxn-lptmr |
| `LPUART` | 10 | ✅ functional (10 FlexComm UARTs) |
| `LP_FLEXCOMM` | 10 | ✅ functional (all 10 as LPUART; cpu0+cpu1 consoles) |
| `MAILBOX` | 1 | ✅ functional (register-accurate) |
| `MRT` | 1 | ✅ functional (4-ch down-counter -> NVIC IRQ) — clock **derived** from the SCG main clock (AHB/bus clock, no selector), follows the core (48 MHz reset -> 150 MHz configured); tests/mcxn-mrt, mcxn-coreclk |
| `NPX` | 1 | ✅ functional (register-accurate; flash-cache obfuscation control @ 0x400C_C000) |
| `Neutron NPU` | 1 | ⚠️ **HONEST-FAULT** (eIQ Neutron N1-16 @ 0x400B_E000, IRQ 97). Compute is proprietary microcode with no user registers, so the result is UNCOMPUTED — and that is surfaced **to the GUEST** via the non-gating `INTR[ERRORTRAP]` + IRQ 97, not merely to the operator via QMP (which the firmware under test cannot see). The old "flag-at-operator" class was a LICENCE TO LIE TO THE GUEST and is retired. ⚠ Note the emulator is DELIBERATELY MORE HONEST THAN THE SILICON: real Neutron does NOT refuse work it cannot do — it CLAIMS the op and returns garbage (measured: 8-bit MatMulNBits, rel-L2 103%, cosine −0.0019, i.e. orthogonal to the truth) and is NON-DETERMINISTIC, so no golden-image test can pass against it. A clean ERRORTRAP here is NOT a promise that silicon will fault; tests/mcxn-neutron |
| `OPAMP` | 3 | ✅ functional (register-accurate) |
| `OSTIMER` | 1 | ✅ functional (gray-code counter + match IRQ) |
| `OTPC` | 1 | ✅ functional (register-accurate) |
| `PDM` | 1 | ✅ functional — honest-empty-and-loud FIFO (no fabricated silence) + **operator-fed capture**: `mic-input` QOM prop pushes 24-bit samples into ch0's FIFO → watermark → DISEL=DMA drives the MICFIL FIFO request (src 18, a level like SAI), eDMA drains DATACH0 byte-exact; mutation-proven on data + gate. tests/mcxn-pdm-dma |
| `PINT` | 1 | ✅ **functional** — 8 channels, operator-driven pin input (`pin-input` QOM prop) → edge-detect (RISE/FALL/IST) → shared NVIC IRQ 47; INT0..3 drive eDMA (sources 3..6, one-shot pulse per edge). Was a register stub (IST always 0, no IRQ). Mutation-proven on DMA + rate + NVIC; tests/mcxn-pint-dma. Edge mode only (level-sensitive is a stated boundary) |
| `PKC` | 1 | ✅ functional (register-accurate) |
| `PLU` | 1 | ✅ functional (register-accurate) |
| `PORT` | 6 | ◐ pin-mux stub (adequate) |
| `POWERQUAD` | 1 | ✅ functional + active (compute-launch -> completion IRQ 76 to NVIC; tests/mcxn-powerquad) |
| `PUF` | 1 | ✅ functional (register-accurate) |
| `PWM` | 2 | ✅ **carrier verified** — period from INIT/VAL1 and **CTRL[PRSC]** (the prescaler was NOT MODELLED AT ALL: an 8× slower carrier request produced the same frequency, and carrier frequency IS motor control), measured against SysTick under -icount and swept across prescalers; tests/mcxn-pwm. **Carrier clock now DERIVED** from the SCG main clock (bus/IPBus clock, no hardcoded 150 MHz) — follows the core (48 MHz reset → 150 MHz configured), mutation-proven. **Value-register DMA** (SM0.DMAEN[VALDE] → reload-driven request, FlexPWM0 Val0=43/FlexPWM1 Val0=51) so PWM_SetupPwmDMA streams duty words into VALx; swept on DATA + RATE axes, mutation-proven both; tests/mcxn-flexpwm-dma. **Input capture** (operator `capture-a-input` QOM edge → CVAL0 + CA0DE-gated capture DMA, source 39/47), mutation-proven on capture-DMA + edge-select; tests/mcxn-flexpwm-capture. **Output trigger → ADC** (motor-control synchronous sampling): TCTRL[OUT_TRIG_EN] bit n → VALn compare pulses PWM_OUT_TRIG0 (even n) / TRIG1 (odd n) MID-CARRIER (its own compare timer; the reload timer only knows the period edge), routed via INPUTMUX (SM0 selectors 24/25, 32/33) to the ADC's HTEN-gated trigger; mutation-proven on the pulse + OUT_TRIG_EN via a SysTick RATE floor; tests/mcxn-adc-pwm-trig. Scope: SM0, full-rate (TRGFRQ seam). |
| `QDC` | 2 | ✅ functional (register-accurate; quadrature decoder) + hardware trigger: a timer/PWM through QDCn_TRIG captures the encoder position into the hold registers (CTRL2[UPDHLD], a coherent PWM-synchronised snapshot) or clears it (CTRL2[UPDPOS]); mutation-proven; tests/mcxn-qdc-trig. Position advancement (PHASEA/PHASEB decode) is a stated input seam |
| `RTC` | 1 | ✅ functional + active (live 1 Hz calendar tick + alarm match -> IRQ 52 to NVIC; tests/mcxn-rtc) |
| `SAI` | 2 | ✅ **real data path** — 8-word TX/RX FIFOs, byte-exact audio over the board-level TXD→RXD jumper, real overrun/underrun, and a WORD RATE verified against SysTick and swept on BOTH axes (TCR2[DIV] and TCR5[W0W]). **MCLK now DERIVED** from SYSCON SAI0CLKSEL/CLKDIV (PLL0/ExtClk/FRO_HF/PLL1÷div, no hardcoded 12.288 MHz) — the word rate follows the selected source (FRO_HF vs PLL0 ratio = 150/48, mutation-proven). Drives eDMA request source 100/99; tests/mcxn-sai, mcxn-sai-dma |
| `SCG` | 1 | ✅ functional — sources (FRO12M/FRO_HF) + **PLL0/PLL1 derived** from APLL/SPLL NDIV/MDIV/PDIV (RM formula), muxed by SYSCON to CTIMER/SCT. **The M33 core clock + SysTick are derived too**: the SCG main clock (RCCR[SCS]) feeds cpuclk/refclk — core boots at 48 MHz FRO_HF, rises to 150 MHz on PLL0; a reconfigure moves SysTick (mcxn-coreclk). tests/mcxn-pll-ctimer, mcxn-coreclk. (Timing tests configure the clock first, like BOARD_InitBootClocks.) |
| `SCT` | 1 | ✅ functional + active (running counter -> periodic match/limit event IRQ 33 to NVIC; tests/mcxn-sct) + **event-paced eDMA** (DMA0=19/DMA1=20, gated by DMAREQ0/1[DEV_n], one-shot pulse per event), mutation-proven on data + rate axes; tests/mcxn-sct-dma. **Input-conditioned events**: an operator-driven SCT input pin edge (`sct-inputs` QOM) fires an event whose EV[n].CTRL selects COMBMODE=IO/IOSEL/IOCOND (in the active state) -> EVFLAG[n] + EVEN[n] IRQ; mutation-proven on flag/IOSEL/IOCOND; tests/mcxn-sct-input. Scope: IO-only events (output/state-machine actions are a boundary) |
| `SEMA42` | 1 | ✅ functional (register-accurate) |
| `SINC` | 1 | ✅ **computes** — a real CIC filter (H(z) = ((1−z^−OSR)/(1−z^−1))^ORD), verified against the closed-form DC gain OSR^ORD across a shape sweep. `SR` used to be hardwired 0x1F00, which hung the stock SDK (MCLKRDY=0) and faked an endless zero-stream (FIFOEMPTY=0); IRQ 142; tests/mcxn-sinc |
| `SMARTDMA` | 1 | ⚠️ **HONEST-FAULT** — the EZH core is not modelled, so the program never runs and NOTHING IS MOVED. `CTRL[START]` therefore stays SET (a dead coprocessor) instead of self-clearing, which used to tell a polling guest its transfer had COMPLETED while the destination buffer was untouched |
| `SPC` | 1 | ✅ functional |
| `SYSCON` | 1 | ✅ functional — CPUCTRL/CPBOOT cpu1 handover + the peripheral clock muxes (CTIMER/SCT/OSTIMER/SAI selectors) + the **AHB busclk** = SCG mainclk/(AHBCLKDIV+1) feeding the core/SysTick/MRT/PWM; tests/mcxn-ahbclkdiv |
| `TDET` | 1 | ✅ functional (register-accurate) |
| `TRDC` | 1 | ✅ functional (register-accurate) |
| `TSI` | 1 | ✅ operator-driven (per-channel count via `tsi-countN` QOM prop; end-of-scan IRQ 101) + **hardware-trigger scan**: an LPTMR through INPUTMUX TSI_TRIG paces the scan in GENCS[STM] mode (EOSF + operator count) — was a fabricated scan-on-STM-write; mutation-proven; tests/mcxn-tsi-trig |
| `USBDCD` | 1 | ✅ functional — runs the real BC1.2 detection sequence (contact→primary→secondary), classifies an operator-driven port (none/SDP/CDP/DCP), mutation-proven (tests/mcxn-usbdcd) |
| `USBFS` | 1 | ✅ functional + active **device mode** (KHCI BDT endpoint engine; enumerates AND moves bulk data both directions end-to-end over usbredir to a remote USB host; IRQ 50; tests/mcxn-usb) + **HOST mode** (CTL[HOSTMODEEN] gives the controller its own usb-bus; a QEMU `usb-kbd` attaches and the guest KHCI host driver enumerates it — device descriptor + SET_ADDRESS — driving each token to the device via usb_handle_packet; CTL[RESET] → usb_device_reset for addressability; real-data + reset mutation-proven; tests/mcxn-usb-host) + **HOST mass storage** (reads AND writes real blocks on an attached usb-storage over SCSI READ(10)/WRITE(10) Bulk-Only Transport (bulk-IN + bulk-OUT + the async .complete path), verifying actual disk-image bytes AND re-checking the backing file on the host after exit; handles the UNIT-ATTENTION-on-first-command retry; tests/mcxn-usb-host-msc) |
| `USBHS1_PHY_DCD` | 1 | ✅ functional (register-accurate; HS phy/dcd 0x800 window) |
| `USBHS1__USBC` | 1 | ✅ functional + active **device mode** (ChipIdea dQH/dTD endpoint engine; enumerates + bulk data both ways at high-speed over usbredir; IRQ 67; tests/mcxn-usb-hs) |
| `USBHS1__USBNC` | 1 | ✅ functional (register-accurate; HS non-core 0xE00 window) |
| `USBPHY` | 1 | ✅ functional (register-accurate; CLKGATE/SFTRST clear, SET/CLR/TOG) |
| `USDHC` | 1 | ✅ **real data path** — drives a genuine QEMU `sd-card` on an sd-bus; real ADMA2 descriptor walk + SDMA + PIO; an EMPTY SLOT TIMES OUT (CTOE) instead of the host answering for it. (It previously CONJURED ITS OWN CARD — invented CMD8/CMD3/ACMD41 responses — and claimed "ADMA block data" with no DMA at all; tests/mcxn-usdhc) |
| `UTICK` | 1 | ✅ functional (register-accurate) |
| `VBAT` | 1 | ✅ functional (register-accurate) |
| `VREF` | 1 | ✅ functional (register-accurate) |
| `WUU` | 1 | ✅ functional (register-accurate) |
| `WWDT` | 2 | ✅ functional (register-accurate) |
## Depth phase: active behaviour on top of the register-accurate models

Breadth is done — every base is modelled. **Depth is well under way, and it is the
phase that matters**, because:

> ⚠ **"REGISTER-ACCURATE" IS NOT THE SAME AS CORRECT, AND IT IS NOT EVEN SAFE.**
> A register-accurate model can hold every offset, reset value and W1C bit exactly
> right *and still lie to firmware*. Ours did, repeatedly. Every one of these
> passed review by reading, and every one was caught only by breaking the model on
> purpose (`tests/mutate.sh`):
>
> - **uSDHC** advertised "ADMA block data" with **no DMA, no descriptor walk and no
>   storage** — it could not move one byte — and it **conjured its own SD card**.
>   Its test passed the mutation audit because *the model was its own oracle*.
> - **eFlexPWM** never modelled `CTRL[PRSC]` at all: a driver asking for an 8×
>   slower carrier got the same one. **Carrier frequency *is* motor control.**
> - **FlexCAN** did no ID matching on the board-to-board path (a driver trusting
>   its own filter read *another node's frame*) and **silently dropped** a frame
>   when no mailbox was free.
> - **eDMA's `CH_CSR[ERQ]` was a dead bit** — peripheral-triggered DMA, how nearly
>   all real audio/ADC/UART transfer works, **did not exist**.
> - **SAI** claimed a word rate "derived from `TCR2[DIV]`" that **nothing tested**.
>
> **A block is done when its test CAN FAIL in the dimension it claims** — not when
> firmware stops hanging. See the Guardrails in `CLAUDE.md`.

The **generated capability table in `README.md`** (from
`docs/validation/test-matrix.yaml`, gated by `tests/check-matrix-drift.sh`) is the
authority on what is actually proven. **Read that, not this heading.**

### Known gaps, stated rather than papered over

- **DMA request lines exist for SAI and DAC only.** ADC, PDM, SINC and the
  LPFlexcomm serials have **none**, so their DMA-driven stock drivers would hang.
  Request-mux sources are in `include/hw/dma/mcxn_edma.h` (`MCXN_DMA_REQ_*`).
- **EMVSIM is retracted** (registers/IRQs only): a smartcard interface needs a
  card, and unlike `sd-card` / `m25p80` / `at24c` there is no card model upstream.
- **The clock tree is COMPLETE — fully derived through the core.** SCG computes its
  sources (FRO12M/FRO_HF) *and PLL0/PLL1* from the divider registers; SYSCON muxes them to
  CTIMER/SCT/OSTIMER/**SAI** and derives the AHB **busclk** = mainclk/(AHBCLKDIV+1); and the
  M33 core clock + SysTick + **MRT** + the **PWM** carrier all follow it (48 MHz FRO_HF at
  reset → 150 MHz on PLL0, and ÷AHBCLKDIV). **LPTMR** is on its RM-Table-463 PSR[PCS] source
  (FRO_12M/FRO_16K/32K/OSC_SYS). The only remaining absolute-frequency item is the
  **external-codec SAI MCLK** — a board seam (off-chip crystal), not an on-chip assumption.
  Ratios are exact throughout; prefer a ratio test where the clock cancels; where it can't (a
  derivation proof), the golden must come from the SDK/RM source rates, not the model.

### eDMA byte-access audit (fleet lesson: silent-drop of narrow DMA bursts)
A peripheral driven by BOTH the CPU (32-bit) and eDMA (byte/halfword bursts to a
FIFO data register) MUST accept sub-word MMIO, or the memory core silently drops
the DMA bytes (transfer "completes", no data moves) — a top-class silent-wrong
bug (origin: 95emulator's LPSPI-over-eDMA case).
- ✅ **FlexComm (LPUART/LPSPI/LPI2C)** — fixed (`.valid/.impl min_access_size=1`);
  proven a 1-byte write to LPSPI TDR now transfers. Handlers dispatch to specific
  registers, no corrupting generic default.
- ✅ **SAI, ADC** — already accept byte access.
- ✅ **DAC, PDM, SINC** — fixed. Each now accepts 1/2/4-byte access
  (`.valid/.impl min_access_size = 1`) and its generic write default does a
  sub-word read-modify-merge instead of overwriting, so a byte/halfword access
  can't corrupt a config register. DAC DATA is a halfword eDMA write-target;
  PDM/SINC result channels are eDMA read-targets (RO, read 0 — no synthesized
  audio). Verified: a DAC config-reg byte-overwrite merges (`0xAABBCCDD` →
  `0xAABBCC11`), and sub-word access to all three data paths is accepted with no
  fault/rejection.

Done so far (active behaviour + IRQ to NVIC + bare-metal test):
- [x] **ADC** — SW-trigger conversion -> RESFIFO valid result + EOC IRQ (45/46). `tests/mcxn-adc`.
- [x] **FlexCAN** — loopback TX MB -> RX MB + IFLAG IRQ (62/63). `tests/mcxn-flexcan`.
- [x] **ENET** — MDIO PHY read (link-up/ID) + DMA soft-reset + PHYIS IRQ (139). `tests/mcxn-enet`.
- [x] **RTC** — live 1 Hz QEMUTimer calendar tick + alarm match IRQ (52). `tests/mcxn-rtc`.
- [x] **uSDHC** — SD command/response handshake (CMD8/CMD3/ACMD41) + CC IRQ (61). `tests/mcxn-usdhc`.
- [x] **FlexSPI** — IP-command-done IRQ (58). `tests/mcxn-flexspi`. eDMA request lines RX=1/TX=2 (IP{RX,TX}FCR[DMAEN]-gated), stock-driver round trip against a real m25p80, mutation-proven both directions. `tests/mcxn-flexspi-dma`.
- [x] **FlexSPI XIP** — AHB-mapped NOR window is real executable memory (NS 0x8000_0000 / secure alias 0x9000_0000, 8 MiB W25Q64 per the N947 DTS). Code linked into the window runs in place (no copy to SRAM); the `-kernel` loader fills it. `tests/mcxn-xip` boots from internal flash and calls a routine executing at 0x9000_0000.
- [x] **FlexSPI QSPI boot** — `-machine frdm-mcxn947,qspi-boot=on` resets the M33 from the external NOR (secure XIP alias 0x9000_0000) with NO internal-flash image (production execute-in-place boot). The boot ROM's go/no-go gate is modelled: a valid FlexSPI Config Block (tag "FCFB" = 0x42464346 at NOR offset 0x400) must be present or the image is rejected (loud + fatal), exactly as on silicon. `tests/mcxn-qspi-boot` boots+runs in place from the NOR and rejects an FCB-less image; mutation-proven on both the FCB check and the init-svtor reroute. Seam (stated): the ROM's FlexSPI *configuration* from the FCB lookup-table is not replayed — only the tag is validated; the XIP read path is covered by mcxn-xip/mcxn-flexspi-nor.
- [x] **SAI** — TX FIFO-request interrupt (FRF & FRIE) IRQ (59/60). `tests/mcxn-sai`.
- [x] **DAC** — FIFO watermark interrupt (FSR.WM & IER.WM_IE) IRQ (106/107/108). `tests/mcxn-dac`.
- [x] **PowerQuad** — compute-launch -> completion IRQ (76). `tests/mcxn-powerquad`.
- [x] **PWM** — submodule-0 running counter -> periodic reload IRQ (114/120, QEMUTimer). `tests/mcxn-pwm`. Value-register DMA on reload (VALDE-gated, Val0=43/51), mutation-proven on data + rate axes. `tests/mcxn-flexpwm-dma`. **Fault protection**: operator-driven FAULT0 (`fault-input` QOM) -> FSTS[FFLAG] latch + FFPIN live-mirror + FIE-gated FLEXPWMn_FAULT IRQ (113/119); the clear-while-active interlock and FLVL polarity are modelled; mutation-proven on detection/FIE/FLVL/interlock. `tests/mcxn-flexpwm-fault`. **Output waveform**: PWM_A(SM0) generated from the counter vs VAL2/VAL3, with OCTRL[POLA] polarity, OUTEN[PWMA_EN] gating, and a DISMAP-mapped latched fault forcing it OFF (the safety cut); operator-observable via the read-only `pwm-a-output` QOM property; 7-axis mutation-proven. `tests/mcxn-flexpwm-output`. **Dead-time + complementary pair**: in complementary mode (CTRL2[INDEP]=0) PWM_B is the complement of PWM_A with dead-time inserted on each leading edge (DTCNT0/DTCNT1, in counts = DTCNT>>PRSC) — during the delay BOTH outputs are inactive (the dead band that stops inverter shoot-through). `pwm-b-output` QOM property; the withdead/nodead discriminator proves the insertion; 3-axis mutation-proven. `tests/mcxn-flexpwm-deadtime`. **PWM_X**: the auxiliary output, set at VAL0 / reset at VAL1 (high in (VAL0,VAL1)), with POLX polarity, OUTEN gate and DISMAP fault force-off; `pwm-x-output` QOM property, 4-axis mutation-proven. `tests/mcxn-flexpwm-pwmx`. Seam: FRACVAL fractional-delay dead-time (a cross-cycle dither, deliberately not faked in the static-position model). **Dangerous-zero reset values FIXED**: DTCNT0/1 = 0x07FF (zero dead-time = DC-bus shoot-through) and DISMAP0 = 0xFFFF (all faults disable all outputs), both RM-verified, were memset to 0 and uncaught. Seam: dead-time INSERTION (DTCNT edge-shift) and complementary-pair/PWM_B/PWM_X output not modelled.
- [x] **SCT** — running counter -> periodic match/limit event-0 IRQ (33, QEMUTimer). `tests/mcxn-sct`.
- [x] **I3C** — controller request -> transfer-complete IRQ (95/96). `tests/mcxn-i3c`.
- [x] **ENET MAC frame path** — DWC ENET-QoS descriptor-ring TX/RX over a real QEMU NIC backend (-nic) + MAC loopback, TI/RI DMA IRQ. `tests/mcxn-enet-mac`. **Cross-board ready.**
- [x] **FlexComm SPI/I2C** — LP_FLEXCOMM PSELID function-select: LPSPI master loopback (TDR->RDR, WCF/FCF/TCF + RDF) and LPI2C controller driving a **REAL I2C bus + at24c EEPROM** (START/addr/TX/RX/STOP, NACK->NDF; write-then-read byte-exact against the device, not an echo), both raising the shared FlexComm NVIC line via ISTAT. `tests/mcxn-flexcomm` (LPSPI on FC3 IRQ 38, LPI2C on FC0 IRQ 35) + `tests/mcxn-lpi2c-eeprom` (dedicated real-EEPROM round-trip).
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
- **Accelerators**: PowerQuad computes real results (matrix/vector + CP0
  transcendentals); unmodelled `CP_MTX` opcodes FAIL to the guest via
  `ERRSTAT[BUSERROR]` instead of leaving a stale result buffer.
  Neutron NPU and SmartDMA are **HONEST-FAULT** — not "flag-at-operator", which
  is DEPRECATED and was the bug: it defined "op acked + truth exposed via QMP"
  as a safe endpoint, and **QMP reaches the OPERATOR, not the firmware under
  test**. Being honest to the host while lying to the guest is not being honest,
  and that rule had already authorised four real bugs here (Neutron, ELS,
  SmartDMA, PowerQuad). Both blocks now fail through their own documented,
  NON-GATING error channel — the guest is told, and is never hung.
- **IRQ wiring**: connect the per-device IRQ lines (init'd in wave 6) to the
  cpu0 NVIC as each block starts generating interrupts.

Pure-config / analog-trim / security blocks (GDET, ITRC, TRDC, ELS, PUF, PKC,
CDOG, VBAT, SPC trims, INPUTMUX, EVTG, etc.) are register-accurate and need no
further dynamics for emulation.
