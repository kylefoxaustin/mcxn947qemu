# MCXN947 model — fidelity audit (what the board farm can / cannot trust)

Per the fidelity-first directive (model targets real-silicon behaviour for
arbitrary developer code), this is the per-block manifest of **how faithfully
each block behaves at runtime**. Shares the format used by the i.MX95 model's
`fidelity-audit.md` so a merged fleet manifest is one document.

## Status taxonomy (best → operator-action → honest-limit)

- **COMPUTES** — does the real work; results match silicon (within emulation).
- **OPERATOR-DRIVEN** — has no physical stimulus in QEMU (analog/sensor); the
  input is exposed as a runtime QOM property (inject the value a board pin would
  drive) instead of a hidden constant. Default = a *documented* test value.
- **FLAG-AT-OPERATOR** — cannot compute (proprietary firmware/ISA) **and**
  cannot fault honestly (the vendor driver has no error path / would hang); so
  the op is acked but the truth is exposed to the farm control-plane via QMP
  (`compute-modelled=false` + an acked-but-uncomputed counter + a LOG_UNIMP).
  **Guests trusting these get wrong results — detectable, not silent.**
- **HONEST-FAULT / ABSENT** — rejects/faults like absent hardware (acceptable).
- **REGISTER-ONLY-OK** — register-accurate; no compute is *expected* of the
  block (config/cache/ID blocks), so there is nothing to get wrong.

## Accelerators / compute blocks (the silent-wrong-answer risk class)

| Block | Status | Notes |
|-------|--------|-------|
| PowerQuad — matrix/vector (MMIO) | **COMPUTES** | mult/add/sub/scale/transpose/dot-product, Q15/Q31/float32; real operands → OUTBASE (commit 7241fdc5c1). |
| PowerQuad — transcendentals (sin/cos/sqrt/ln/exp/div) | ⚠ **HARD-FAULTS today** | These use the custom ARM **CP0/CP1 coprocessor** (MCR/MRC), which QEMU's M33 doesn't implement → NOCP→HardFault. Fix = Phase 2 (target/arm coprocessor). Until then, PowerQuad-math/CMSIS-DSP-on-PQ code crashes — **do not rely on it yet.** |
| SmartDMA (EZH coprocessor) | **FLAG-AT-OPERATOR** | Runs a firmware program we don't execute. `qom-get …/smartdma compute-modelled` = false; `programs-started` counts acked-uncomputed starts; LOG_UNIMP per start. |
| eIQ Neutron NPU | ⚠ **NOT MODELLED** | The 0x400C_C000 window models **NPX (flash-cache)**, not the Neutron compute register set (absent from CMSIS). eIQ/LiteRT inference would **not run / not compute**. Treat ML inference as unsupported on this model. |

## Analog inputs (no physical stimulus in QEMU)

| Block | Status | Notes |
|-------|--------|-------|
| ADC0/1 (LPADC) | **OPERATOR-DRIVEN** | Conversion result = `adc_ch[channel]` selected by the triggered command (TCTRL→CMD→ADCH), not a constant. Inject: `qom-set …/adc0 adc-ch5 2748`. Default = documented mid-scale 0x800. |
| CMP0/1/2 (comparator) | ◐ **constant (TODO operator-driven)** | Output not yet operator-exposed — same pattern as ADC to follow. |
| TSI (touch sense) | ◐ **constant (TODO operator-driven)** | Per-channel count is a constant; operator-driven counts to follow. |
| DAC0/1/2 | REGISTER-ONLY-OK | Output sink (FIFO watermark IRQ modelled); no analog read-back to falsify. |

## Functional / honest blocks (compute or move real data correctly)

GPIO, PORT pin-mux, LPUART console, **FlexComm LPSPI/LPI2C** (loopback/echo),
FlexCAN (MB loopback), **ENET** (real DWC descriptor-ring frames over a QEMU
NIC), uSDHC (SD cmd/resp), FlexSPI (IP-cmd-done), SAI (TX-request), RTC (live
1 Hz), eDMA (really moves data), CTIMER/SCT/PWM/MRT/OSTIMER/LPTMR (real
counters), EMVSIM (TX-complete), I3C (transfer-complete), inter-CPU MAILBOX
(real cross-core IRQ), **ELS** (crypto exercised by els_pkc examples; TRNG
returns real entropy) — all compute or move real data; safe to trust.

## Honest register-only (no compute expected)

Security/config/cache/ID blocks: SYSCON, SPC, SCG, GDET, ITRC, TRDC, PUF, PKC,
CDOG, VBAT, INPUTMUX, EVTG, PLU, FREQME, NPX cache, etc. — register-accurate,
nothing to compute, nothing to get wrong.

## Bottom line for the farm
Trust the functional + COMPUTES blocks. For OPERATOR-DRIVEN analog, inject
inputs via QOM. **Do not trust** (and the control-plane can detect via QMP/log):
PowerQuad transcendentals (until Phase 2), SmartDMA program output, and Neutron
NPU inference.
