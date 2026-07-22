# CLAUDE.md — MCX N947 QEMU machine model (build agent: mcxqemu)

## Mission

Integrate, build, and bring up a QEMU machine model for the **NXP MCXN947**
(dual Arm Cortex-M33) into a local QEMU checkout, then iterate peripheral
support. Target machine: `frdm-mcxn947`. Keep the code clean, self-contained,
and in-tree-style — all model logic in the `mcxn_*` files, no edits to generic
QEMU. This is a private model in `kylefoxaustin/mcxn947qemu`; nothing here is
submitted anywhere.

The human (Kyle) maintains the i.MX 93/95/91 QEMU models. Reuse that experience:
the patterns here mirror the i.MX work, and the console UART is the **same
LPUART IP** as i.MX 93/95.

## Source of truth

- Device facts (IRQ count, prio bits, peripheral bases, bit masks) come from the
  MCXN947 CMSIS header. Do **not** invent addresses — pull them from:
  `https://raw.githubusercontent.com/nxp-mcuxpresso/mcux-sdk/main/devices/MCXN947/MCXN947_cm33_core0.h`
  (or the local MCUXpresso SDK copy on this machine).
- Code is written against **current QEMU mainline** idioms. If the local tree is
  older, see "Version touchpoints" below.

## Layout

The model is **in-tree**: this repo *is* a QEMU checkout. ~75 `mcxn_*` device
models under `hw/`, the SoC in `hw/arm/mcxn_soc.c`, the board in
`hw/arm/mcxn_frdm.c`, and 58 test suites under `tests/mcxn-*/`, each with a
self-contained `run.sh` that builds its own firmware and asserts on the output.

```
hw/arm/mcxn_soc.c            SoC: cores, memories, every peripheral + its secure alias
hw/arm/mcxn_frdm.c           frdm-mcxn947 board (sysclk 150 MHz, canbus/ENET/SPI links)
hw/*/mcxn_*.c                the peripheral models
tests/mcxn-*/                one directory per suite: main.c + link.ld + run.sh
tests/mutate.sh              ⭐ mutation harness — break the model, prove the test notices
tests/gen-test-matrix.py     generates the README capability table from test-matrix.yaml
tests/check-matrix-drift.sh  CI gate: the table must match the yaml
docs/validation/             test-matrix.yaml = SOURCE OF TRUTH for capability claims
PERIPHERALS.md               per-block status
```

## Build + run

```sh
./configure --target-list=arm-softmmu     # first time only
ninja -C build                            # ~seconds after the first build
bash tests/mcxn-gpio/run.sh               # any single suite
for d in tests/mcxn-*/; do bash "$d/run.sh"; done   # the lot (~20 min)
bash tests/mcxn-ztest/run.sh              # 24 Zephyr ztest suites (needs staged ELFs)
```

⚠ **Do not rebuild while a suite is running.** `ninja` replaces the binary
underneath it and the results are garbage — this has invalidated three separate
"definitive" runs. Hash the binary before and after if the numbers matter.

⚠ **Any timing measurement needs `-icount shift=3`.** Without it virtual time
tracks *host* time and a golden compared against a noisy measurement gives a
confident, reproducible-looking, WRONG answer.

## Bring-up iteration loop

The peripheral window is a catch-all `unimplemented` region. Every access into
unmodelled space is logged by `-d unimp,guest_errors`. That log is the
prioritised to-do list:

1. Run firmware, capture the unimp log.
2. Identify the first peripheral the firmware blocks on.
3. Implement it as a `SysBusDevice`, map it (it overrides the catch-all by
   memory-region priority), connect its IRQ via
   `qdev_get_gpio_in(armv7m, <IRQn>)`.
4. Rebuild, rerun, repeat.

Pull register layouts and IRQ numbers from the CMSIS header, exactly as the
LPUART model was built.

## Verified MCXN947 facts

| Property           | Value                                             |
|--------------------|---------------------------------------------------|
| Core               | dual Cortex-M33 @ 150 MHz (MVP wires cpu0 only)    |
| Core features      | FPU, DSP, MPU, SAU/TrustZone-M                     |
| NVIC external IRQs | 156 (highest CTI0_IRQn = 155)                      |
| __NVIC_PRIO_BITS   | 3                                                 |
| Flash              | 2 MiB @ 0x0000_0000                               |
| SRAM               | 512 KiB @ 0x2000_0000                             |
| Console            | FlexComm4 / LPUART4 @ 0x400B_4000, IRQ 39          |
| FlexCAN            | CAN0 @ 0x400D_4000, CAN1 @ 0x400D_8000            |
| Neutron NPU        | IRQ 97; base from RM (not in CMSIS header)         |

TrustZone-M: peripherals are aliased non-secure @ `0x400x_xxxx` and secure @
`0x500x_xxxx`. The catch-all spans `0x4000_0000..0x5FFF_FFFF` to cover both.

## Version touchpoints (check first if the build fails)

Written for current mainline. On an older QEMU tree, adjust:

1. **`serial_hd` include** — mainline: `system/system.h`; older:
   `sysemu/sysemu.h`. (in `hw/arm/mcxn_soc.c`)
2. **`device_class_set_legacy_reset`** — newer API. Older trees:
   `dc->reset = mcxn_lpuart_reset;`. (in `hw/char/mcxn_lpuart.c`)
3. **Property arrays** — mainline dropped the `DEFINE_PROP_END_OF_LIST()`
   terminator. If the compiler complains, add it back to the two `Property[]`
   arrays.
4. **`armv7m_load_kernel(cpu, name, mem_base, mem_size)`** — the `mem_base`
   (3rd) arg is relatively recent. Older signature drops it. (in `hw/arm/mcxn_frdm.c`)
5. **`ARMV7M` clock inputs** are `cpuclk` / `refclk` on mainline (confirmed).

## Status

**The bring-up phase is long over.** Both M33s boot, Zephyr runs (ztest 25/25,
409 cases), the stock MCUXpresso example corpus runs, and the board is a live node
on five board-to-board transports (ethernet / UART / USB / SPI / CAN). The
**3-node L2 lab passes**: MCX (M33/ENET-QoS) + i.MX RT1180 (M33/NETC) + i.MX 95
(A55, real Linux, ENETC) exchanging raw frames on one wire.

**The capability table in `README.md` is GENERATED from
`docs/validation/test-matrix.yaml` and gated by `tests/check-matrix-drift.sh`.
That table is the status. Read it, and do not hand-edit it.**

### Real open items (stated, not papered over)

- **DMA request lines: SAI, DAC, ADC, the LPFlexcomm serials, SINC, FlexSPI,
  FlexPWM (value-register / reload path), CTIMER (match M0/M1), and SCT (event → DMA0/1)
  are wired** (each drives its CMSIS request-mux source; sources in
  `include/hw/dma/mcxn_edma.h`). A CTIMER match / SCT event is a one-shot **pulse** (not a
  FIFO level), so the eDMA has an edge path that auto-acks a pulse after one minor loop and
  drops an unconsumed one — reused by any match/event source.
  **PDM/MICFIL is now operator-fed**: a mic bitstream has no source in emulation, so the
  `mic-input` QOM property pushes 24-bit samples into channel 0's FIFO; with CTRL_1[DISEL]=
  DMA the FIFO watermark drives the request (a LEVEL, like the SAI) and the eDMA drains
  DATACH0 (mcxn-pdm-dma). **FlexPWM's CAPTURE DMA is the last input-seam — a capture request
  needs an input edge on the PWM pins, which has no signal source in emulation, so the gap
  is the missing input.**
  **HsCmp/CMP is now operator-driven**: its crossing has no analog source, but it was
  already exposed as the `comparator-output` QOM property, so `CCR1[DMA_EN]` redirecting an
  IER-enabled edge to the DMA request just needed wiring — the operator injects a crossing
  over QMP and it paces the eDMA (mcxn-cmp-dma). **PinInt is now functional + operator-
  driven**: PINT was a register stub (IST/RISE always 0, no IRQ); building it out earned the
  pin-interrupt path (operator `pin-input` QOM property → edge-detect → PINT0_IRQn=47) and
  its DMA (INT0..3 → sources 3..6), mutation-proven on DMA + rate + NVIC (mcxn-pint-dma).
  **FlexPWM capture — the last seam — is now closed**: the FlexPWM captures its counter into
  CVAL0 on an operator-driven input-A edge (`capture-a-input` QOM property, CAPTCTRLA[ARMA]+
  EDGA0 select the edge), and DMAEN[CA0DE] pulses the capture request (sources 39/47) through
  the edge path (mcxn-flexpwm-capture, mutation-proven on capture-DMA + edge-select).
  **Every eDMA request source is now wired** — the peripheral-triggered-DMA campaign is
  complete (input seams closed via operator QOM inputs: analog crossing / pin edge / mic
  bitstream / PWM-pin capture edge).
- **EMVSIM is retracted** (tier B): a smartcard interface needs a card, and unlike
  `sd-card`/`m25p80`/`at24c` there is no card model upstream. An ISO-7816 card is
  roadmap. A stated gap is honest; a badge over one is a bug.
- **The clock tree is mostly modelled — including the CORE clock.** The derived slice:
  SCG computes its sources (FRO12M, FRO_HF) *and PLL0/PLL1* from the APLL/SPLL
  NDIV/MDIV/PDIV registers via the RM formula ((48/8)×50/2 = 150 MHz); SYSCON muxes all of
  these — including the PLLs (CTIMER selectors 1/2, SCT selectors 1/4) — to CTIMER/SCT/
  OSTIMER, which FOLLOW a PLL reconfigure (mcxn-pll-ctimer). **The M33 core clock + SysTick
  are now DERIVED too**: the SCG main clock (RCCR[SCS]) feeds cpuclk/refclk, so an
  un-configured core boots at the real **48 MHz FRO_HF** reset rate and rises to 150 MHz
  once firmware brings up PLL0 — a reconfigure moves SysTick with it (mcxn-coreclk).
  ⚠ **Because the core boots at 48 MHz (not 150), a timing test that measures absolute time
  against SysTick MUST configure the clock first, exactly as `BOARD_InitBootClocks` does**
  (the `clock_init_150m()` preamble in the pwm/mrt/sct/flexpwm-dma tests) — or it measures
  the 48 MHz reset clock and fails. This is the honest reset-clock path; the "boot
  pre-configured to 150 MHz" shortcut is BLOCKED by the reset-values gate (it correctly
  refuses to fake non-RM SCG reset values). **MRT is now derived too** — it runs on the
  AHB/bus clock = the SCG main clock (no selector, `kCLOCK_Mrt` is an AHB gate), so it
  follows the core (mcxn-mrt proves MRT and SysTick share the derived clock at the 48 MHz
  reset rate; wiring MRT back to a constant makes it read ~64000 vs 200000 → FAIL). **PWM is
  now derived too** — the FlexPWM counter takes a Clock input off the SCG main clock (its
  IPBus clock is the bus/core clock), so the motor-control carrier follows the core (48 MHz
  reset → 150 MHz configured) instead of a hardcoded `PWM_IPBUS_HZ`; mcxn-pwm proves it at
  the reset clock (PWM and SysTick share the derived clock, so the tick-count golden holds).
  **SAI is now derived too** — its MCLK takes a Clock input off SYSCON SAI0/1CLKSEL/CLKDIV
  (CLOCK_GetSaiClkFreq: PLL0 / ExtClk / FRO_HF / PLL1÷div), so the audio rate follows the
  source firmware selects (the classic 12.288 MHz is now a real PLL1 config, not a hardcode);
  mcxn-sai proves the MCLK follows the selector (FRO_HF vs PLL0 word-rate ratio = 150/48
  exactly, mutation-proven — a constant MCLK gives 1.0 and fails). The external-codec MCLK
  (SAI as clock consumer) remains a board seam.
  **Still assumed** (each with a real blocker, not laziness): **LPTMR** currently runs on the
  150 MHz sysclk, which is WRONG — it ignores PSR[PCS] and should run on a low-power clock;
  fixing it needs the RM's PCS→clock table (the SDK enum is generic "clock 0/1/2/3", it does
  NOT name the sources, so guessing them would be fabrication). **AHBCLKDIV** is unmodelled
  (core/MRT/PWM take mainclk directly = assume /1); routing them through a SYSCON busclk needs
  SYSCON realized before the cores (an ordering change). Ratios are exact throughout; these
  absolute frequencies are the documented assumption. Say which.

### What "done" means here (learned the hard way)

A block is not done when it acks. It is done when a test **can fail** in the
dimension it claims. **Run `tests/mutate.sh` on any claim you add** — it breaks the
model on purpose and refuses to score a mutation that did not compile. Claims that
looked solid and were **decoration**: uSDHC "ADMA block data" (it had no DMA at all
and conjured its own SD card), the eFlexPWM carrier (the prescaler was *not
modelled* — a driver asking for an 8× slower carrier got the same one), the SAI
word rate (ignoring `TCR2[DIV]` left the suite green). Every one passed review by
reading.

## Guardrails

**North star: real-silicon fidelity for arbitrary developer code (a 10k-developer
virtual board farm). A SILENT WRONG ANSWER IS THE TOP-TIER BUG.** A model that
hangs gets diagnosed in an hour. A model that returns a confident, plausible,
wrong number ships into somebody's product.

- **Never fabricate a register value, offset, base or IRQ.** Derive it from the
  CMSIS header or the RM. ⚠ And watch your own vocabulary: **"plausible",
  "nominal", "reasonable", "best-effort" are the words you use when you mean
  FABRICATED.** Grepping the tree for them found a made-up FIFO depth, an invented
  version register, and a PWM tick rate that appears nowhere in the RM.
- **Never invent a peer.** No device on the other end? Expose the **seam** and let
  the operator wire it — an attachable bus (I3C supplies the I2C bus, the *test*
  attaches the EEPROM; FlexSPI drives a real `m25p80`; uSDHC drives a real
  `sd-card`), a board-level property (SAI's TXD→RXD jumper), or an operator-driven
  QOM input (ADC/DAC/CMP/TSI). **Do NOT conjure a peer and call the result a data
  path** — uSDHC did, and its test passed the mutation audit because *the model was
  its own oracle*. **But first check whether the peer already exists:** `-device
  help`. "No peer" is a conclusion, not a starting assumption.
- **Fail to the GUEST, not just the log.** QMP and `qemu_log` reach the *operator*;
  the firmware under test cannot see them. **Being honest to the host while lying
  to the guest is not being honest.** Fault through the block's own documented,
  **non-gating** error channel (never the completion gate — that hangs the driver
  instead of informing it).
- **A test is not done when it passes. It is done when it CAN FAIL.** Run
  `tests/mutate.sh`. Reading a test tells you what it *checks*, never what it can
  *catch*.
- **Sweep every axis you claim.** One shape is a collapsed oracle: a 2×2·2×2 matrix
  hides a dimension swap; a fixed word length hides an ignored `W0W`; an
  un-prescaled PWM hides a prescaler that isn't modelled at all.
- **A screen is not a verdict.** Greps proposed; only *reading the code* disposed —
  five times in one session, every one of my sweeps produced false positives.
- **Verify, don't infer.** Check the build's *exit status*, not its output. Assert
  your edit anchor matched (`str.replace` silently no-ops). An **empty result is
  not a pass**, and a **killed run is not a caught bug**.
- Report the unimp log back after a firmware run so the peripheral order is driven
  by real firmware behaviour, not guesswork.
