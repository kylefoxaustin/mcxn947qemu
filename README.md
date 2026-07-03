# qemu-mcxn947

A QEMU machine type for the NXP **MCX N947** microcontroller — a **dual Arm
Cortex-M33** MCU — targeting the **FRDM-MCXN947** board. Machine name:
`frdm-mcxn947`.

> **This is a fork of QEMU mainline.** The MCX N work lives on the `mcxn947`
> branch; the vast majority of the history is inherited from upstream QEMU. The
> upstream QEMU README is preserved at [`README.rst`](README.rst) — this file
> describes the MCX N-specific work. All model logic is self-contained in new
> `mcxn_*` files (no edits to generic QEMU), with the long-term aim of being
> upstream-mergeable.

qemu-mcxn947 is a QEMU model of the NXP MCXN947. Unlike the sibling i.MX 9x
ports, this is a **microcontroller, not an applications processor**: there is no
Linux and no MMU-class OS. You run the same firmware you would flash to the
silicon — **bare-metal, Zephyr, or the MCUXpresso SDK** — against a
register-accurate model of the whole chip, so you can develop, debug, and
CI-test MCX N firmware without hardware.

It boots the Zephyr **`frdm_mcxn947`** target and passes the upstream Zephyr
**`ztest`** suite (kernel + IPC + the userspace/MPU/SAU TrustZone-M path). Every
peripheral base on the chip has a register-accurate functional model, and the
blocks whose behaviour firmware can actually observe have live data paths and
NVIC interrupts on top. Highlights:

- **Dual Cortex-M33** — cpu0 boots and releases cpu1 (SYSCON `CPUCTRL`/`CPBOOT`),
  with a working **inter-core MAILBOX** and an **OpenAMP/RPMsg-style** shared-
  memory ring transport (real message data cpu0↔cpu1, not just a doorbell).
- **USB device mode** — both controllers (USBFS/KHCI and USBHS/ChipIdea) present
  real USB devices over the `usbredir` protocol to a remote USB *host*. A
  **CDC-ACM serial gadget** enumerates on stock Linux (`cdc_acm` → `/dev/ttyACM0`)
  and round-trips bytes — proven against real i.MX 93 **and** i.MX 91 QEMU hosts.
- **Board-to-board / inter-QEMU links** — the MCX is a drop-in node on
  **ENET, UART, USB, and SPI** links over a chardev socket, so it pairs with the
  i.MX 91/93 QEMU models (and a lab coordinator) over a real bus.
- **DSP / accelerators** — PowerQuad (matrix/vector engine + CP0 scalar
  transcendentals, computing real results) and the eIQ **Neutron NPU** (modelled
  honestly: the compute handshake is acked so firmware never hangs, but the
  proprietary-microcode result is flagged uncomputed via QMP rather than
  silently fabricated).
- **XIP boot** — the FlexSPI AHB NOR window is real executable memory; code
  linked there runs in place.
- **Networking** — ENET with a descriptor-ring MAC path over a QEMU NIC backend
  (Zephyr's stack gets a DHCP lease and completes a TCP echo).

Intended use cases: firmware and peripheral-driver development, multicore /
RPMsg bring-up, board-to-board interconnect development, and CI. It is **not
cycle-accurate** — it models register and data-path behaviour, not timing.

**Maintainer:** Kyle Fox ([@kylefoxaustin](https://github.com/kylefoxaustin))

## Building

```sh
./configure --target-list=arm-softmmu
make -j"$(nproc)"          # or: ninja -C build qemu-system-arm
```

## Running your firmware

```sh
./build/qemu-system-arm \
    -M frdm-mcxn947 \
    -kernel your-firmware.elf \
    -nographic \
    -serial mon:stdio
```

- **`-kernel`** takes an ELF (or raw binary). Code linked at flash `0x0000_0000`
  runs from there; code linked into the FlexSPI XIP window at `0x8000_0000`
  (secure `0x9000_0000`) runs in place.
- **Console:** the FRDM debug console is **FlexComm4 / LPUART4** = `serial_hd(0)`,
  so `-serial mon:stdio` gives you console + the QEMU monitor. The second core's
  console (**FlexComm2 / LPUART2**) is `serial_hd(1)` — add a second `-serial`
  for it.
- **Early output before any UART is set up:** semihosting works from the first
  instruction: `-semihosting-config enable=on,target=native`.
- **See any access into unmodelled space** (rare — the whole peripheral window is
  modelled): `-d unimp,guest_errors`. The peripheral map is
  highest-priority-wins: register-accurate models sit over a catch-all backstop.

A Zephyr `hello_world` built for `frdm_mcxn947/mcxn947/cpu0` is the quickest
"it works" image; the console prints over FlexComm4.

## Inter-QEMU / board-to-board links

The MCX plugs into the fleet's link fabric over a chardev socket — one QEMU per
"board", byte-exact — so you can wire an MCX to an i.MX 91/93 (or another MCX)
over a real transport:

| Link | MCX side | How it attaches |
|------|----------|-----------------|
| **USB** | USBFS/USBHS device gadget over `usbredir` | a stock `-device usb-redir` client on the host QEMU |
| **UART** | FlexComm2/LPUART2 on a socket chardev | `-serial chardev:<sock>` on each side |
| **SPI**  | FlexComm5 LPSPI as an SSI-bus master | `-device spi-link,bus=mcxn-lpspi,chardev=<sock>` |
| **ENET** | descriptor-ring MAC over a QEMU NIC | `-nic socket,...` / a QEMU netdev between the two |

See `tests/mcxn-usb-*`, `tests/mcxn-uart-link`, and `tests/mcxn-spi-link` for
runnable examples of each.

## Testing

Each modelled peripheral has a small **bare-metal test** under `tests/mcxn-*`
(build a tiny firmware, run it, assert the console output). For example:

```sh
tests/mcxn-rpmsg/run.sh       # dual-core shared-memory IPC
tests/mcxn-usb-cdc/run.sh     # USB CDC-ACM enumerate + bulk echo
tests/mcxn-spi-link/run.sh    # SPI board-to-board
```

Full firmware coverage comes from the Zephyr **`frdm_mcxn947`** port and its
`ztest` suite. `PERIPHERALS.md` tracks per-peripheral coverage and status.

## MCXN947 — verified facts (CMSIS `MCXN947_cm33_core0.h`)

| Property            | Value                                              |
|---------------------|----------------------------------------------------|
| Cores               | dual Arm Cortex-M33 @ up to 150 MHz               |
| Core features       | FPU, DSP, MPU, SAU / TrustZone-M                    |
| NVIC external IRQs  | **156** (highest `CTI0_IRQn` = 155)                |
| `__NVIC_PRIO_BITS`  | **3**                                              |
| Flash               | 2 MiB @ `0x0000_0000`                              |
| SRAM                | 512 KiB @ `0x2000_0000` (banked RAMA..H, contig.)  |
| Console UART        | FlexComm4 / LPUART4 @ `0x400B_4000`, IRQ 39        |
| Inter-CPU mailbox   | `0x400B_2000`, MAILBOX IRQ 54 (per-core)           |

**TrustZone-M aliasing:** every peripheral is mapped twice — non-secure at
`0x400x_xxxx` and secure at `0x500x_xxxx`. Firmware may use either alias.

## Memory map

| Region            | Base          | Size    | Notes                              |
|-------------------|---------------|---------|------------------------------------|
| Code flash        | `0x0000_0000` | 2 MiB   | RAM-backed; `-kernel` loads here   |
| SRAM              | `0x2000_0000` | 512 KiB | banked, mapped contiguous          |
| Peripherals (NS)  | `0x4000_0000` | —       | register-accurate models           |
| Peripherals (S)   | `0x5000_0000` | —       | secure TrustZone-M alias           |
| FlexSPI NOR (NS)  | `0x8000_0000` | 8 MiB   | AHB-mapped external flash (XIP)    |
| FlexSPI NOR (S)   | `0x9000_0000` | 8 MiB   | secure alias of the XIP window     |
| PPB (NVIC/SysTick)| `0xE000_0000` | —       | handled by the `ARMV7M` container  |

## What is *not* modelled (honestly)

The goal is real-silicon fidelity for arbitrary firmware, and **silent wrong
answers are treated as the worst class of bug**. Where a block cannot be
computed faithfully, it is register-accurate and flags the gap rather than
fabricating a result:

- **Neutron NPU** and **SmartDMA** run proprietary microcode that isn't
  modelled — the control handshake completes (no hang) but the result is flagged
  *uncomputed* via QMP `qom-get`, never silently wrong.
- Timing is not modelled (not cycle-accurate).
- Pure-config / analog-trim / security blocks (GDET, ITRC, TRDC, ELS, PUF, PKC,
  CDOG, VBAT, SPC trims, INPUTMUX, EVTG, …) are register-accurate.

## Repository layout

The model lives in the standard QEMU tree under `mcxn_*`-prefixed files:

| Area | Files |
|------|-------|
| SoC + board | `hw/arm/mcxn_soc.c`, `hw/arm/mcxn_frdm.c`, `include/hw/arm/mcxn_soc.h` |
| Console / FlexComm (UART/SPI/I2C) | `hw/char/mcxn_lpuart.c` |
| USB device core + engines | `hw/usb/mcxn_usbdev.c`, `hw/misc/mcxn_usbfs.c`, `hw/misc/mcxn_usbhs.c` |
| Clocks / SYSCON / accelerators / … | `hw/misc/mcxn_*.c` |
| Per-peripheral bare-metal tests | `tests/mcxn-*/` |
| Coverage + status tracker | `PERIPHERALS.md` |

## Building against a different QEMU base

Written against current QEMU mainline. On an older tree, check:

- `serial_hd` lives in `system/system.h` (older: `sysemu/sysemu.h`).
- `ARMV7M` clock inputs are `cpuclk` / `refclk`.
- `armv7m_load_kernel(cpu, filename, mem_base, mem_size)` — the `mem_base` arg
  is relatively recent.
- `device_class_set_props()` / `DEFINE_TYPES()` are the current idioms;
  `Property[]` arrays no longer need `DEFINE_PROP_END_OF_LIST()`.
