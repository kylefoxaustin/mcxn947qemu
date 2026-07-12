# qemu-mcxn947

A QEMU machine type for the NXP **MCX N947** microcontroller — a **dual Arm
Cortex-M33** MCU — targeting the **FRDM-MCXN947** board. Machine: `frdm-mcxn947`.

> **This is a fork of QEMU mainline.** The MCX N work lives on the `mcxn947`
> branch; the bulk of the history is upstream QEMU. The upstream QEMU README is
> preserved at [`README.rst`](README.rst) — this file describes the MCX N-specific
> work. All model logic is self-contained in new `mcxn_*` files (no edits to
> generic QEMU), with the long-term aim of upstream-mergeability.

qemu-mcxn947 is a QEMU model of the NXP MCXN947, and one node in a fleet of NXP
QEMU ports (i.MX 91 / 93 / 95 and this MCXN947 microcontroller) that share device
models and a validation standard. Unlike the i.MX 9x siblings, this is a
**microcontroller, not an applications processor**: there is no Linux and no
MMU-class OS. You run the same firmware you would flash to the silicon —
**bare-metal, Zephyr, or the MCUXpresso SDK** — against a register-accurate model
of the whole chip. It is not cycle-accurate.

It boots the Zephyr **`frdm_mcxn947`** target and passes the upstream Zephyr
**`ztest`** suite (kernel + IPC + the userspace / MPU / SAU TrustZone-M path).
Every peripheral base on the chip has a register-accurate model; the blocks whose
behaviour firmware can observe have live data paths and NVIC interrupts on top.
Beyond running on one board, it passes real data between instances over five board
buses (see [Interconnect](#interconnect--board-to-board-mission-5)). Intended use:
firmware and peripheral-driver development, multicore / RPMsg bring-up, multi-board
lab work, and CI; the long-term aim is upstream-mergeability into QEMU mainline.

**Maintainer:** Kyle Fox ([@kylefoxaustin](https://github.com/kylefoxaustin))

![qemu-mcxn947 — dual Cortex-M33 MCU, the fleet's real-time node](docs/images/mcxn947-hero.png)

*Part of a consistent hero-image family across the QEMU fleet — solid silicon for
the emulator repos, one accent per node (MCXN947 cobalt-blue). The two glowing
dies are the dual Cortex-M33 cores; the USB connector is the CDC-ACM gadget that
pairs with the Linux boards.*

## Quickstart

This fork **builds and runs as-is** — a plain clone lands on `mcxn947`.

**1. Clone and build** (host packages under [Building](#building)):

    git clone https://github.com/kylefoxaustin/mcxn947qemu.git
    cd mcxn947qemu
    ./configure --target-list=arm-softmmu
    make -j"$(nproc)"                                  # or: ninja -C build qemu-system-arm
    ./build/qemu-system-arm -M help | grep frdm-mcxn947   # -> frdm-mcxn947

**2. Run your firmware.** The MCX is bare-metal — you supply a `-kernel` ELF (no
Linux, no DTB), exactly the image you would flash to the board:

    ./build/qemu-system-arm -M frdm-mcxn947 -kernel your-firmware.elf \
        -nographic -serial mon:stdio

Three details are load-bearing:

- **Console = FlexComm4 / LPUART4** is `serial_hd(0)`, so `-serial mon:stdio` gives
  you console + the QEMU monitor. The second core's console (FlexComm2 / LPUART2)
  is `serial_hd(1)`.
- **Semihosting works from the first instruction**, before any UART is set up:
  add `-semihosting-config enable=on,target=native` for early output.
- **`-kernel` runs code linked at flash `0x0000_0000`**; code linked into the
  FlexSPI XIP window at `0x8000_0000` (secure `0x9000_0000`) runs in place.

A Zephyr `hello_world` built for `frdm_mcxn947/mcxn947/cpu0` is the quickest
"it works" image; the console prints over FlexComm4. `-d unimp,guest_errors`
surfaces any access into unmodelled space (rare — the whole peripheral window is
modelled; register-accurate models sit over a catch-all backstop).

## What runs today

The Zephyr **`frdm_mcxn947`** target boots and the **`ztest`** suite passes on the
dual Cortex-M33. Every peripheral base is register-accurate; this table is the
condensed capability view, with the per-IP-block evidence and the detailed class
language in
[`docs/validation/test-result-matrix.md`](docs/validation/test-result-matrix.md)
(one source of truth, `test-matrix.yaml`, two renderings). Tiers: **A** data-path
verified (real data moves / real math, integrity-checked) · **B** register-accurate
bring-up (binds, registers / IRQ / timing correct; analog inputs operator-driven;
proprietary accels honestly flagged) · **N/A** absent on MCXN947 silicon.

<!-- BEGIN capability-table (generated from test-matrix.yaml) -->
| Subsystem | Tier | Evidence |
|---|:--:|---|
| Dual Cortex-M33 (cpu0 boot + cpu1 release), NVIC/SysTick | A | cpu0 releases cpu1 via SYSCON CPUCTRL/CPBOOT; both run (tests/mcxn-dualcore) |
| Inter-core MAILBOX + RPMsg-style shared-memory ring | A | Real message data cpu0<->cpu1, byte-exact (tests/mcxn-mailbox, mcxn-rpmsg) |
| Console + FlexComm (LPUART / LPSPI / LPI2C) | A | Zephyr console; SPI/I2C master + board-to-board links (tests/mcxn-flexcomm, mcxn-spi-link, mcxn-uart-link) |
| USB device — USBFS/KHCI + USBHS/ChipIdea, CDC-ACM | A | Enumerates on stock Linux cdc_acm -> /dev/ttyACM0, bulk byte-exact (tests/mcxn-usb-cdc) |
| Networking — ENET (descriptor-ring MAC) | A | Zephyr stack: DHCP lease + TCP echo over a QEMU NIC (tests/mcxn-enet*) |
| FlexCAN x2 | A | Loopback + board-to-board frame round-trip (tests/mcxn-can-link) |
| SAI + I3C + EMVSIM — registers/IRQs only, NO DATA PATH YET | B | RETRACTED from tier A on 2026-07-12: mutation-testing showed these three move no data at all (SAI RDR/TDR, I3C MRDATAB, EMVSIM RX_BUF are stubs), and their tests asserted only that an interrupt fired — so a corrupted-data mutation could not fail them. Drivers bind and IRQs are correct; a data path is roadmap |
| Storage / XIP — uSDHC + FlexSPI NOR | A | uSDHC ADMA block data; FlexSPI drives a real SPI-NOR (LUT/IP commands: WREN + erase + page program, bits only 1->0) with a byte-exact erase -> program -> read-back round trip, and code runs in place from the same array via the XIP window, which refuses CPU stores (tests/mcxn-usdhc, mcxn-flexspi-nor, mcxn-xip) |
| Timers / PWM — CTIMER, MRT, LPTMR, OSTIMER, SCT, eFlexPWM, RTC | A | Timer/PWM data paths + IRQs (tests/mcxn-ctimer, mcxn-timers, mcxn-ostimer, mcxn-sct, mcxn-pwm, mcxn-rtc) |
| GPIO + eDMA | A | GPIO toggles; eDMA TCD transfers (tests/mcxn-gpio, mcxn-dma) |
| PowerQuad DSP (matrix/vector + CP0 transcendentals) | A | Computes real results — matrix/vector ops + scalar sin/cos/ln/divide |
| Audio out — DAC output FIFO | A | DAC drives a real output FIFO — occupancy, FULL/EMPTY/watermark, overflow drops the sample, underflow holds the output, and a level IRQ deasserts on refill (tests/mcxn-dac) |
| SINC sigma-delta filter (computes) | A | Real CIC: the RM's H(z) = ((1-z^-OSR)/(1-z^-1))^ORD decimates a register-fed (PM/SM) modulator bitstream to a 24-bit result, checked against the filter maths (tests/mcxn-sinc) |
| Flash program — FMU (storage-write-verified) | A | Byte-exact erase -> program -> read-back round-trip driven through the RM PEWEN/PERDY sequence; flash is a ROM device, so stores outside a program window are refused and cumulative programming fails verify (tests/mcxn-fmu) |
| Security — ELS entropy (TRNG); crypto HONESTLY FAULTED, never faked | A | TRNG is real: fresh entropy per read and via RND_REQ DMA, driving Zephyr stack_random (ztest userspace path). The crypto engine (AES/HASH/HMAC/CMAC/ECDSA/ECDH/key-derivation) is NOT implemented and is NOT faked: every such command fails through the engine's own error channel (ELS_STATUS[ELS_ERR] + ELS_ERR_STATUS[OPN_ERR]) and leaves the result buffer untouched, so firmware is told rather than handed uninitialised memory as a signature or key. BUSY still clears, so no driver hangs (tests/mcxn-els) |
| Watchdogs + micro-tick — WWDT, EWM, UTICK | B | Register-accurate; reset/refresh/timeout semantics |
| Accelerators (honest) — Neutron NPU, SmartDMA, PowerQuad fixed-point | B | Nothing is fabricated and nothing hangs: the Neutron NPU flags its UNCOMPUTED result to the GUEST (INTR[ERRORTRAP] + IRQ 97), SmartDMA leaves CTRL[START] set because its program never ran and no data was moved, and PowerQuad's unmodelled opcodes fail via ERRSTAT[BUSERROR] rather than leaving a stale result. Host-only flags (QMP/log) are NOT sufficient — the firmware under test cannot see them (tests/mcxn-neutron, mcxn-powerquad-coproc) |
| Analog & audio-in — ADC, CMP, TSI, OPAMP, VREF, PDM | B | Register-accurate; analog inputs operator-driven via QOM property. PDM has no bitstream source in emulation and says so: it produces NO samples and flags FIFO underflow rather than fabricating silence firmware cannot tell from real audio |
| Pin / IRQ / GPIO infra — PORT, INPUTMUX, PINT, INTM, EVTG, FLEXIO, QDC, PLU | B | Drivers bind; pin-mux / IRQ routing registers correct (sub-word MMIO) |
| Memory / cache / CRC — CACHE64, NPX, CRC, SEMA42, OTPC | B | Register-accurate; CRC compute, cache/ID/fuse config |
| Clocks / power / system — SCG, SYSCON, SPC, CMC, VBAT, WUU, FREQME, AHBSC | B | Clock/power config; firmware programs directly (no System Manager) |
| Security / crypto / tamper — PKC, PUF, CDOG, GDET, ITRC, TRDC, TDET | B | Drivers bind; registers / reset values / W1C semantics correct |
| USB support — USBDCD, USBPHY, USBHS-NC | B | Charger-detect / PHY / non-core config registers |

**Absent on MCXN947 silicon — N/A (never a failure):**

| Block | Why absent |
|---|---|
| Cortex-A55 · Linux-capable MMU · apps-processor OS | It's an MCU — real-time, bare-metal / RTOS / Zephyr, no Linux |
| LCDIF · MIPI-DSI · HDMI bridge · camera ISI/CSI | No display/camera pipeline on this MCU |
| System Manager (SM/SCMI) | MCU has none; firmware programs SCG/SYSCON clocks directly |
| Ethos-U65 NPU | The MCX carries the eIQ Neutron NPU instead (flagged, honest) |
| External DDR controller | On-chip SRAM (512 KiB) + FlexSPI NOR (XIP); no DRAM |
<!-- END capability-table (generated from test-matrix.yaml) -->

**TrustZone-M** — every peripheral is mapped twice: non-secure `0x400x_xxxx` and
secure `0x500x_xxxx`. Firmware may use either alias.

## Interconnect — board-to-board (mission #5)

Beyond running on one board, the MCX **passes real data between QEMU instances**
over its buses, in the per-link socket shape a lab coordinator
([holobench](https://github.com/kylefoxaustin/holobench)) wires — so two emulated
boards hook up over a stock QEMU socket, no host kernel/root. Every link has a
byte-exact oracle, and each has been cross-validated against a **real** i.MX
sibling (Linux master ↔ bare-metal M33). Harnesses: `tests/mcxn-*-link`.

| Transport | Shape | Status |
|---|---|:--:|
| **Ethernet** | ENET descriptor-ring MAC over a QEMU NIC / socket netdev | PASS |
| **UART** | FlexComm2/LPUART2 on a `-chardev socket` | PASS |
| **USB** | USBFS/USBHS gadget over `usbredir` (bulk + CDC-ACM `/dev/ttyACM0`) | PASS (2 hosts) |
| **SPI** | FlexComm5 LPSPI via the **`spi-link`** device (91's) over `-chardev socket` | PASS (vs imx91/93 Linux `fsl-lpspi`) |
| **CAN** | FlexCAN via **`can-host-chardev`** (95's), `-machine canbus0=cb` | PASS (vs imx95 Linux SocketCAN) |

The shared bridge devices — `spi-link` (i.MX 91), `can-host-chardev` (i.MX 95) —
are carried verbatim, so the MCX wires into a mixed lab with the **identical
incantation** as the Linux boards; the CAN wiring uses the fleet-standard
`-machine canbus0=cb,canbus1=cb`.

## Validation

Correctness rests on **five independent gates**, not one:

1. **Zephyr `ztest`** on `frdm_mcxn947` — 24 suites (~409 cases): functional
   kernel + IPC + the userspace / MPU / SAU secure path (incl. `stack_random`
   exercising the ELS TRNG entropy). The primary firmware-driven gate.
2. **Per-peripheral bare-metal tests** (`tests/mcxn-*`) — each modelled block has a
   tiny firmware that drives it and asserts console output (data-path + IRQ).
3. **MCUXpresso example corpus** — NXP's stock SDK examples build and run against
   the model (the PORT sub-word-MMIO bug that killed every `BOARD_InitPins` was
   found this way).
4. **Interconnect + cross-SoC** — byte-exact board-to-board over five transports,
   each cross-validated against the **real** i.MX 91 / 93 / 95 nodes (Linux master
   ↔ bare-metal M33).
5. **Boot + machine smoke** — machine registration, semihosting, and console
   over FlexComm4 from a bare-metal image.

The recurring lesson mirrors the fleet's: a green deterministic test is not
validation for a data path a real driver exercises differently — the streaming-RX
LPUART fix and the eDMA byte-access (`min_access_size`) fix only surfaced against
real-driver / cross-SoC repros. Fidelity judgments live in
[`docs/validation/fidelity-audit.md`](docs/validation/fidelity-audit.md); overall
coverage in [`PERIPHERALS.md`](PERIPHERALS.md).

## Required artifacts

The MCX is a microcontroller — it runs **firmware**, not a Linux stack, so the
Linux/DTB/rootfs artifacts the i.MX siblings need are **N/A** here:

| Artifact | Where from |
| --- | --- |
| **Firmware ELF** (bare-metal) | any Arm bare-metal toolchain (`arm-none-eabi-gcc`); see `tests/mcxn-*` |
| **Zephyr image** (optional) | Zephyr `frdm_mcxn947` board (`west build -b frdm_mcxn947/mcxn947/cpu0`) |
| **MCUXpresso SDK image** (optional) | NXP MCUXpresso SDK for MCXN947 |
| ~~Linux `Image`~~ | N/A — no Linux on this MCU |
| ~~Device tree (`.dtb`)~~ | N/A — bare-metal, no DTB |
| ~~rootfs / initramfs~~ | N/A — no userspace OS |

The `tests/*/run.sh` scripts build their firmware with `arm-none-eabi-gcc` and take
`QEMU=`/`CC=` env vars; they `SKIP` cleanly if the toolchain is absent.

## Building

    ./configure --target-list=arm-softmmu
    make -j"$(nproc)"                     # or: ninja -C build qemu-system-arm

**Host packages (Ubuntu 22.04+):**

    sudo apt install -y \
        meson ninja-build python3 python3-venv python3-tomli \
        gcc libc6-dev pkg-config libglib2.0-dev libpixman-1-dev \
        gcc-arm-none-eabi          # for building the bare-metal test firmware

## Architecture overview

- **2× Cortex-M33** (FPU, DSP, MPU, SAU/TrustZone-M) via the `ARMV7M` object —
  cpu0 boots and releases cpu1 (SYSCON CPUCTRL/CPBOOT). NVIC: 156 external IRQs,
  3 priority bits. No Cortex-A55, no MMU-class OS — it is an MCU.
- **Memory:** 2 MiB code flash @ `0x0000_0000` (RAM-backed for bring-up), 512 KiB
  SRAM @ `0x2000_0000` (banked RAMA..H, mapped contiguous), FlexSPI NOR XIP window
  @ `0x8000_0000` (secure `0x9000_0000`). Peripherals `0x4000_0000` (NS) /
  `0x5000_0000` (secure TrustZone-M alias); PPB (NVIC/SysTick) handled by `ARMV7M`.
- Real device models for everything firmware exercises — the FlexComm block
  (LPUART/LPSPI/LPI2C function-select), USB device engines (KHCI + ChipIdea),
  ENET, FlexCAN, eDMA, timers (CTIMER/SCT/eFlexPWM/LPTMR/MRT/OSTIMER), uSDHC,
  FlexSPI, the analog blocks (ADC/DAC/CMP/TSI/SAI/PDM/SINC), PowerQuad DSP, the
  Neutron NPU + SmartDMA (honest), SCG/SYSCON/SPC clocks, MAILBOX, RTC, and the
  security cluster — plus the `spi-link` / `can-host-chardev` interconnect devices.
- **Source of truth:** every base address, IRQ number, and bit mask comes from the
  MCXN947 CMSIS header (`MCXN947_cm33_core0.h`) and the MCX N Reference Manual,
  never guessed — a wrong offset is a silent firmware hang. Structural conventions
  follow the i.MX LPUART model (the console is the same NXP LPUART IP as i.MX 93/95).

## Repository tour

| Path | Purpose |
| --- | --- |
| `hw/arm/mcxn_soc.c`, `include/hw/arm/mcxn_soc.h` | SoC realization: dual M33, memories, device wiring, per-SKU config table |
| `hw/arm/mcxn_frdm.c` | `frdm-mcxn947` board (custom machine type; clocks, kernel load, `canbus0/1` links) |
| `hw/char/mcxn_lpuart.c` | FlexComm — LPUART/LPSPI/LPI2C function-select console + master engines |
| `hw/usb/mcxn_usbdev.c`, `hw/misc/mcxn_usbfs.c`, `hw/misc/mcxn_usbhs.c` | USB device core + KHCI/ChipIdea engines (usbredir gadget) |
| `hw/misc/mcxn_*.c` | clocks, SYSCON/SPC, timers, analog, PowerQuad, Neutron/SmartDMA, security, MAILBOX, FlexCAN |
| `hw/ssi/spi_link.c`, `net/can/can_host_chardev.c` | the board-to-board **interconnect** transports (SPI + CAN chardev bridges) |
| `hw/arm/Kconfig`, `hw/*/meson.build` | `MCXN_SOC` config + build wiring |
| `tests/mcxn-*/` | per-peripheral bare-metal tests; `mcxn-{uart,spi,can}-link` + `mcxn-usb-cdc` are the board-to-board harnesses |
| `docs/validation/` | the test-result matrix, fidelity audit, and `test-matrix.yaml` (class source of truth) |
| `PERIPHERALS.md` | full per-peripheral coverage + the depth program + the eDMA byte-access audit |

## Known limitations

- **PDM/MICFIL has no microphone** — a PDM bitstream has no source in emulation and,
  unlike the SINC, the RM gives MICFIL no register-fed input. The model therefore
  produces **no samples at all** and flags `FIFO_STAT[FIFOUNDn]` on a read, rather
  than handing back an endless zero-stream firmware could not tell apart from real
  audio. Enabling it logs `LOG_UNIMP`. Driving it from an operator-supplied PCM
  source is roadmap; the FIFO/watermark/IRQ machinery is already in place.
- **DAC periodic-trigger (`GCR[PTGEN]`) and swing-back (`GCR[SWMD]`) are not
  modelled** — enabling either logs `LOG_UNIMP`; drive the FIFO with `TCR[SWTRG]`.
  Everything else about the DAC (occupancy, FULL/EMPTY/watermark, overflow drop,
  underflow hold, read/write pointers) is real.
- **SINC's external modulator pins (MBIT/INP) have no source** — selecting them logs
  `LOG_UNIMP` and yields no samples. The RM's register-fed **PM/SM** modes
  (`CnCFR[IBFMT] = 10b/11b`) drive the real CIC, and are what the model computes with.
- **Neutron NPU / SmartDMA run proprietary microcode that is not modelled** — the
  control handshake completes (firmware never hangs) but the result is flagged
  *uncomputed* via QMP, never silently fabricated.
- **LPSPI reports 4 chip-selects but does not decode `TCR.PCS`** to select among
  multiple slaves on one bus — fine for one-slave-per-controller and the b2b link.
- Analog inputs (ADC/CMP/TSI/DAC/…) have no physical stimulus in QEMU; values are
  operator-driven via QOM properties (honest, never a fabricated reading).
- Not cycle-accurate (TCG); no silicon timing is implied by any throughput.

## Roadmap & milestone history

The model is **breadth-complete** — every peripheral base is register-accurate and
real firmware (Zephyr + `ztest`, the MCUXpresso corpus) runs without an unmodelled
hang — extended this cycle with the **board-to-board interconnect** (five
transports, cross-SoC validated against real i.MX 91/93/95) and the fleet's
**eDMA byte-access fidelity fix**. What remains is **depth** (active data paths on
the analog/audio blocks a workload can observe; the DAC/PDM/SINC byte-access
follow-up) and **upstream submission** (the machine + board + the `mcxn_*` device
models). A behavioural Neutron NPU command-stream executor (bit-exact int8, the way
the i.MX 93/95 Ethos-U65 is modelled) is the largest open depth item.

## License

GPL-2.0-or-later, matching upstream QEMU. See [`README.rst`](README.rst) and
[`LICENSE`](LICENSE) for the upstream QEMU licensing.
