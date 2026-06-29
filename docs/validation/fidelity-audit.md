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
| PowerQuad — transcendentals (sin/cos/sqrt/ln/exp/div, **float32**) | **COMPUTES** | Phase 2 done (commit pending): the custom ARM **CP0 coprocessor** (MCR/MCRR/MRC) is implemented in target/arm, gated by ARM_FEATURE_POWERQUAD (only the MCXN947 M33s via the "powerquad" CPU property — every other Arm CPU still NOCP-faults CP0). float32 sin/cos/sqrt/invsqrt/inv/ln/etox/etonx + division compute correctly (host libm; the real PowerQuad is an approximation engine, not IEEE-exact). |
| PowerQuad — transcendentals (**fixed-point** Q-format) | ⚠ **HONEST-FAULT** | The fixed-point variants (CRn bit0=1) are deliberately routed to the NOCP fault rather than risk a silently-wrong Q-format result — same as before Phase 2. Float32 is the dominant path (CMSIS-DSP-on-PQ + the PQ_*F32 API). |
| SmartDMA (EZH coprocessor) | **FLAG-AT-OPERATOR** | Runs a firmware program we don't execute. `qom-get …/smartdma compute-modelled` = false; `programs-started` counts acked-uncomputed starts; LOG_UNIMP per start. |
| eIQ Neutron NPU | **FLAG-AT-OPERATOR** | The Neutron N1-16 compute block @ 0x400B_E000 (IRQ 97). RM §20.4: "no user-configurable registers... use eIQ Toolkit" — the compute path is proprietary microcode (SDK binary blobs), so we cannot run it. The CTRL handshake the eIQ driver spins on (exec: while bit31; done: while !=0) is honoured (CTRL idles on write) so inference **does not hang** — but the result is **uncomputed and never silently fabricated**: `qom-get …/neutron0 compute-modelled` = false, `jobs-started` counts acked-uncomputed kicks, LOG_UNIMP per kick. Operator opt-in `uncomputed-errortrap` surfaces the fault to the guest via the non-gating INTR.ERRORTRAP bit + IRQ 97 (never the completion gate). `tests/mcxn-neutron`. (The separate 0x400C_C000 "NPX0" window is the flash-cache block, modelled register-accurate.) |

## Analog inputs (no physical stimulus in QEMU)

| Block | Status | Notes |
|-------|--------|-------|
| ADC0/1 (LPADC) | **OPERATOR-DRIVEN** | Conversion result = `adc_ch[channel]` selected by the triggered command (TCTRL→CMD→ADCH), not a constant. Inject: `qom-set …/adc0 adc-ch5 2748`. Default = documented mid-scale 0x800. |
| CMP0/1/2 (comparator) | **OPERATOR-DRIVEN** | Output level = the `comparator-output` bool QOM prop (what the +/- inputs would resolve to). Setting it drives CSR[COUT], latches the rising/falling edge flags CSR[CFR]/[CFF] and raises the comparator IRQ (109/110/111) when armed. Inject: `qom-set …/cmp0 comparator-output true`. |
| TSI (touch sense) | **OPERATOR-DRIVEN** | A scan latches DATA[TSICNT] from `tsi_count[ CONFIG[TSICH] ]` (per-channel, 25 channels) and raises the end-of-scan IRQ (101). Inject: `qom-set …/tsi0 tsi-count3 1840`. Default = documented sample 0x100. |
| DAC0/1/2 | REGISTER-ONLY-OK | Output sink (FIFO watermark IRQ modelled); no analog read-back to falsify. |

## Functional / honest blocks (compute or move real data correctly)

GPIO, PORT pin-mux, LPUART console, **FlexComm LPSPI/LPI2C** (loopback/echo),
FlexCAN (MB loopback), **ENET** (real DWC descriptor-ring frames over a QEMU
NIC), uSDHC (SD cmd/resp), **FlexSPI** (IP-cmd-done **+ XIP**: the AHB-mapped
NOR window — NS 0x8000_0000 / secure 0x9000_0000, 8 MiB — is real executable
memory, so code linked there boots/runs in place; tests/mcxn-xip), SAI
(TX-request), RTC (live
1 Hz), eDMA (really moves data), CTIMER/SCT/PWM/MRT/OSTIMER/LPTMR (real
counters), EMVSIM (TX-complete), I3C (transfer-complete), inter-CPU MAILBOX
(real cross-core IRQ), **ELS** (crypto exercised by els_pkc examples; TRNG
returns real entropy) — all compute or move real data; safe to trust.

## Inter-QEMU transports (data actually crosses the wire)

| Block | Status | Notes |
|-------|--------|-------|
| USBFS0 (KHCI) **device mode** | **COMPUTES / data-path** | Real BDT endpoint engine: guest firmware's descriptors drive a full USB enumeration AND real bulk data transfer to a remote USB *host* over usbredir (we play the usbredir server; the host runs stock `-device usb-redir`). Verified end-to-end in `tests/mcxn-usb`: EP0 control (GET_DESCRIPTOR/SET_ADDRESS/SET_CONFIGURATION) + EP1 bulk OUT→IN echo (host bytes round-trip through firmware). The OBMF-ICP / i.MX-host↔MCX-device transport (mission #5). |
| USBHS1 (ChipIdea) **device mode** | **COMPUTES / data-path** | Real dQH/dTD endpoint engine on the same shared usbredir core, *high-speed*: enumeration + EP1 bulk echo verified end-to-end in `tests/mcxn-usb-hs`. Each controller has its own usbredir core/socket (`mcxn-usbfs` / `mcxn-usbhs` chardev ids), so both inter-QEMU links are independent. |

## Honest register-only (no compute expected)

Security/config/cache/ID blocks: SYSCON, SPC, SCG, GDET, ITRC, TRDC, PUF, PKC,
CDOG, VBAT, INPUTMUX, EVTG, PLU, FREQME, NPX cache, etc. — register-accurate,
nothing to compute, nothing to get wrong.

## Bottom line for the farm
Trust the functional + COMPUTES blocks (now including PowerQuad's float32
matrix/vector engine AND its scalar transcendentals/division). For
OPERATOR-DRIVEN analog (ADC/CMP/TSI), inject inputs via QOM. **Do not trust**
(and the control-plane can detect via QMP/log): PowerQuad *fixed-point*
transcendentals (honest-fault), SmartDMA program output, and Neutron NPU
inference (FLAG-AT-OPERATOR — compute-modelled=false, jobs-started, optional
guest error-trap).
