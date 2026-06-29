# MCXN947 — test / result matrix

Per-IP-block capability + test status for the `frdm-mcxn947` model. Companion
to `fidelity-audit.md` (the *how-faithfully-it-behaves* axis); this is the
*is-it-tested-and-does-it-pass* axis. Machine-readable twin: `test-matrix.yaml`.

Fleet convergence (operator mission #2/#3): shares the column shape proposed by
95emulator so the 4 boards diff. **Chip-accuracy rule:** a row exists only for
IP the MCXN947 *actually has* — never a false "negative" for absent silicon.
MCX deltas vs the Linux-booting boards: **no Linux-boot / in-guest-build rows**
(the M33 runs cross-built code on-core; the run half is the code-sweep), and an
extra **operator-driven** class for analog inputs injected via QOM.

## Class legend

| Class | Meaning |
|-------|---------|
| **computes** | Does the real work; results match silicon (within emulation). |
| **functional** | Moves real data / generates real events + IRQs on a verified data path. |
| **operator-driven** | No physical stimulus in QEMU; the input is a runtime QOM property (inject what a board pin would drive). Analog. |
| **register-only** | Register-accurate; no compute is *expected* (config / cache / ID / security-trim blocks). Nothing to get wrong. |
| **flag-at-operator** | Cannot compute (proprietary firmware/ISA) and cannot fault honestly; op acked, truth exposed via QMP (no silent-wrong). |
| **not-modelled** | Silicon present but the compute register set is not modelled; treat as unsupported (honest). |

`result` columns are from an actual test run (40/40 mcxn-* suites PASS,
2026-06-29); `class`/`driver-binds` are human fidelity judgments CI cannot infer
from a green test.

## Cores

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| Cortex-M33 cpu0 | ✓ | ✓ | functional | mcxn-zephyr, mcxn-ztest, mcxn-mcuxpresso | PASS |
| Cortex-M33 cpu1 | ✓ | ✓ | functional | mcxn-dualcore, mcxn-mailbox | PASS |
| WFI / idle | ✓ | ✓ | functional | mcxn-idle | PASS |

## Connectivity & data-path peripherals

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| USBFS0 (KHCI, full-speed) | ✓ | ✓ | functional | mcxn-usb (enum + bulk over usbredir) | PASS |
| USBHS1 (ChipIdea, high-speed) | ✓ | ✓ | functional | mcxn-usb-hs (enum + bulk over usbredir) | PASS |
| ENET (Ethernet QoS) | ✓ | ✓ | functional | mcxn-enet, -enet-mac, -enet-tcp/udp/tcpip, -enet-x2, -enet-lab | PASS |
| FlexCAN CAN0/1 | ✓ | ✓ | functional | mcxn-flexcan | PASS |
| LPUART (FlexComm0..9) | ✓ | ✓ | functional | mcxn-multiuart, mcxn-flexcomm | PASS |
| LPSPI (FlexComm) | ✓ | ✓ | functional | mcxn-flexcomm | PASS |
| LPI2C (FlexComm) | ✓ | ✓ | functional | mcxn-flexcomm | PASS |
| I3C0/1 | ✓ | ✓ | functional | mcxn-i3c | PASS |
| uSDHC | ✓ | ✓ | functional | mcxn-usdhc | PASS |
| FlexSPI (+ XIP) | ✓ | ✓ | functional | mcxn-flexspi, mcxn-xip | PASS |
| SAI0/1 (audio) | ✓ | ✓ | functional | mcxn-sai | PASS |
| EMVSIM0/1 (smartcard) | ✓ | ✓ | functional | mcxn-emvsim | PASS |
| MAILBOX (inter-CPU) | ✓ | ✓ | functional | mcxn-mailbox, mcxn-dualcore | PASS |

## Timers

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| CTIMER0..4 | ✓ | ✓ | functional | mcxn-ctimer, mcxn-timers | PASS |
| MRT | ✓ | ✓ | functional | mcxn-timers | PASS |
| LPTMR0/1 | ✓ | ✓ | functional | mcxn-timers | PASS |
| OSTIMER | ✓ | ✓ | functional | mcxn-ostimer, mcxn-timers | PASS |
| SCTimer/PWM | ✓ | ✓ | functional | mcxn-sct | PASS |
| eFlexPWM0/1 | ✓ | ✓ | functional | mcxn-pwm | PASS |
| RTC | ✓ | ✓ | functional | mcxn-rtc | PASS |
| UTICK | ✓ | ◐ | register-only | boot/corpus | PASS |
| WWDT0/1 | ✓ | ◐ | register-only | boot/corpus | PASS |
| EWM | ✓ | ◐ | register-only | boot/corpus | PASS |

## Compute / accelerators

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| PowerQuad (matrix/vector + scalar transcendentals) | ✓ | ✓ | computes | mcxn-powerquad, mcxn-powerquad-coproc | PASS |
| PowerQuad fixed-point transcendentals | ✓ | ✓ | flag-at-operator | mcxn-powerquad-coproc | PASS |
| SmartDMA (EZH coprocessor) | ✓ | ◐ | flag-at-operator | boot/corpus | PASS |
| eIQ Neutron NPU | ✓ | ✗ | not-modelled | — (NPX flash-cache modelled, not Neutron compute) | — |

## Analog (operator-driven)

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| ADC0/1 (LPADC) | ✓ | ✓ | operator-driven | mcxn-adc | PASS |
| CMP0/1/2 (LPCMP) | ✓ | ✓ | operator-driven | mcxn-cmp | PASS |
| TSI (touch) | ✓ | ✓ | operator-driven | mcxn-tsi | PASS |
| DAC0/1/2 | ✓ | ✓ | functional | mcxn-dac | PASS |
| OPAMP0/1/2 | ✓ | ◐ | register-only | boot/corpus | PASS |
| VREF | ✓ | ◐ | register-only | boot/corpus | PASS |
| PDM (mic) | ✓ | ◐ | register-only | boot/corpus | PASS |
| SINC | ✓ | ◐ | register-only | boot/corpus | PASS |

## GPIO / pin / DMA / interrupt

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| GPIO0..5 | ✓ | ✓ | functional | mcxn-gpio | PASS |
| PORT0..5 (pin-mux) | ✓ | ✓ | register-only | mcxn-gpio, mcxn-mcuxpresso (BOARD_InitPins) | PASS |
| eDMA0/1 | ✓ | ✓ | functional | mcxn-dma | PASS |
| INPUTMUX | ✓ | ✓ | register-only | boot/corpus | PASS |
| PINT | ✓ | ◐ | register-only | boot/corpus | PASS |
| INTM | ✓ | ◐ | register-only | boot/corpus | PASS |
| EVTG | ✓ | ◐ | register-only | boot/corpus | PASS |
| FLEXIO | ✓ | ◐ | register-only | boot/corpus | PASS |
| QDC0/1 | ✓ | ◐ | register-only | boot/corpus | PASS |
| PLU | ✓ | ◐ | register-only | boot/corpus | PASS |

## Memory / flash / cache

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| FMU (flash controller) | ✓ | ✓ | functional | mcxn-fmu | PASS |
| CACHE64_CTRL / POLSEL | ✓ | ◐ | register-only | boot/corpus | PASS |
| NPX (flash-cache obfuscation) | ✓ | ◐ | register-only | boot/corpus | PASS |
| CRC | ✓ | ◐ | register-only | boot/corpus | PASS |
| SEMA42 | ✓ | ◐ | register-only | boot/corpus | PASS |
| OTPC | ✓ | ◐ | register-only | boot/corpus | PASS |

## Clock / power / system

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| SYSCON | ✓ | ✓ | register-only | boot/corpus (CPU1 release, SET/CLR) | PASS |
| SCG (clock gen) | ✓ | ✓ | register-only | boot/corpus | PASS |
| SPC (power) | ✓ | ✓ | register-only | boot/corpus | PASS |
| CMC | ✓ | ◐ | register-only | boot/corpus | PASS |
| VBAT | ✓ | ◐ | register-only | boot/corpus | PASS |
| WUU | ✓ | ◐ | register-only | boot/corpus | PASS |
| FREQME | ✓ | ◐ | register-only | boot/corpus | PASS |
| AHBSC | ✓ | ◐ | register-only | boot/corpus | PASS |

## Security / crypto / tamper

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| ELS (crypto + TRNG) | ✓ | ✓ | functional | mcxn-ztest (stack_random/TRNG), corpus els_pkc | PASS |
| PKC | ✓ | ◐ | register-only | boot/corpus | PASS |
| PUF | ✓ | ◐ | register-only | boot/corpus | PASS |
| CDOG0/1 | ✓ | ◐ | register-only | boot/corpus | PASS |
| GDET0/1 | ✓ | ◐ | register-only | boot/corpus | PASS |
| ITRC | ✓ | ◐ | register-only | boot/corpus | PASS |
| TRDC | ✓ | ◐ | register-only | boot/corpus | PASS |
| TDET | ✓ | ◐ | register-only | boot/corpus | PASS |

## USB support blocks

| Block | Present | Driver-binds | Class | Tested-by | Result |
|-------|:---:|:---:|-------|-----------|:---:|
| USBDCD0 (charger detect) | ✓ | ◐ | register-only | boot/corpus | PASS |
| USBPHY / USBHS1 PHY-DCD | ✓ | ◐ | register-only | boot/corpus | PASS |
| USBHS1 non-core (USBNC) | ✓ | ◐ | register-only | boot/corpus | PASS |

## Aggregate harnesses (cross-block)

| Harness | Covers | Tested-by | Result |
|---------|--------|-----------|:---:|
| Zephyr ztest (24 suites / 409 cases) | kernel + IPC + userspace/MPU/SAU + TRNG | mcxn-ztest | PASS |
| Zephyr boot | SoC bring-up, console, clocks | mcxn-zephyr | PASS |
| MCUXpresso corpus (170 examples) | breadth across the SDK | mcxn-mcuxpresso | PASS |
| qtest (device unit tests) | per-device register/IRQ semantics | mcxn-qtest | PASS |
| migration (vmstate save/restore) | state serialization | mcxn-migration | PASS |
| soak (loop all + RSS/counter watch) | stability / leak / coexistence | mcxn-soak | (operator-run) |

## i.MX93 ↔ MCX USB link (M4)

Device end verified standalone (both controllers); the live 2-party pairing
against i.MX93 stock Linux is cross-emulator (holobench-orchestrated). Harness:
`tests/mcxn-usb-link/`.
