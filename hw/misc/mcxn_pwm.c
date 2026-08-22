/*
 * NXP MCX N eFlexPWM (PWM) — bring-up model.  See header for the modelled
 * semantics.  One QOM type ("mcxn-pwm") instantiated for PWM0 and PWM1.
 *
 * Register layout from the MCXN947 CMSIS header (PWM_Type).  Offsets/bit masks
 * are taken verbatim from that header; reset values default to zero (the RM
 * gives 0x0000 reset for the registers exercised at bring-up).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "hw/misc/mcxn_pwm.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"

/* Per-submodule register block: base 0x0, step 0x60, four submodules. */
#define PWM_SM_STEP        0x60
#define PWM_SM_COUNT       4
#define PWM_SM_BASE(n)     ((n) * PWM_SM_STEP)

/* Submodule register offsets within a block. */
#define PWM_SM_INIT        0x02    /* Initial count */
#define PWM_SM_CTRL2       0x04    /* Control 2 (CLK_SEL) */
#define PWM_SM_CTRL        0x06    /* Control (PRSC) */
/* Value register 0 (first of the VAL block) */
#define PWM_SM_VAL0        0x0A
#define PWM_SM_VAL1        0x0E    /* Modulo (period) value */
#define PWM_SM_VAL2        0x12    /* PWM_A turn-ON compare */
#define PWM_SM_VAL3        0x16    /* PWM_A turn-OFF compare */
/* PWM_B turn-ON compare (independent mode) */
#define PWM_SM_VAL4        0x1A
/* PWM_B turn-OFF compare / last of the VAL block */
#define PWM_SM_VAL5        0x1E
#define PWM_SM_OCTRL       0x22    /* Output Control (POLA/POLB/POLX) */
#define PWM_SM_STS         0x24    /* Status (W1C flags) */
#define PWM_SM_INTEN       0x26    /* Interrupt enable */
#define PWM_SM_DMAEN       0x28    /* DMA Enable */
/* Fault Disable Mapping 0 (reset 0xFFFF, RM Table 54.5.23) */
#define PWM_SM_DISMAP0     0x2C
/* Deadtime Count 0 (reset 0x07FF, RM 54.5.24) */
#define PWM_SM_DTCNT0      0x30
/* Deadtime Count 1 (reset 0x07FF, RM 54.5.25) */
#define PWM_SM_DTCNT1      0x32
/* Output Trigger Control (OUT_TRIG_EN / TRGFRQ) */
#define PWM_SM_TCTRL       0x2A
#define PWM_SM_CAPTCTRLA   0x34    /* Capture Control A (arm + edge select) */
/* Capture Value 0 (input A, capture circuit 0), RO */
#define PWM_SM_CVAL0       0x40

/*
 * TCTRL[OUT_TRIG_EN] (CMSIS PWM_TCTRL_OUT_TRIG_EN, mask 0x3F): bit n makes
 * the VALn compare emit an output trigger -- even n -> PWM_OUT_TRIG0, odd n
 * -> PWM_OUT_TRIG1.
 */
#define PWM_TCTRL_OUT_TRIG_EN_MASK 0x3Fu

/*
 * OCTRL[POLA]/[POLB]/[POLX]: invert PWM_A/PWM_B/PWM_X output polarity
 * (CMSIS, bits 10/9/8).
 */
#define PWM_OCTRL_POLA     0x0400u
#define PWM_OCTRL_POLB     0x0200u
#define PWM_OCTRL_POLX     0x0100u
/*
 * OUTEN[PWMA_EN] bits[11:8] / [PWMB_EN] bits[7:4] / [PWMX_EN] bits[3:0];
 * SM0 = bit 8 / 4 / 0.
 */
#define PWM_OUTEN_PWMA_SM0 0x0100u
#define PWM_OUTEN_PWMB_SM0 0x0010u
#define PWM_OUTEN_PWMX_SM0 0x0001u
/*
 * DISMAP0[DIS0A] bits[3:0] / [DIS0B] bits[7:4] / [DIS0X] bits[11:8], one
 * per fault -> PWM_A/B/X; fault 0 = bit 0 / bit 4 / bit 8.  Reset 0xFFFF =>
 * every fault disables every output.
 */
#define PWM_DISMAP_DIS0A_F0 0x0001u
#define PWM_DISMAP_DIS0B_F0 0x0010u
#define PWM_DISMAP_DIS0X_F0 0x0100u
/*
 * CTRL2[INDEP] (bit 13): 0 = complementary pair (deadtime-capable), 1 =
 * independent A/B.
 */
#define PWM_CTRL2_INDEP    0x2000u
/*
 * Reset values (RM register-summary table; memset(0) would ship the
 * dangerous zeros).
 */
#define PWM_DTCNT_RESET    0x07FFu
#define PWM_DISMAP_RESET   0xFFFFu
/*
 * SM CTRL resets to 0x0400 = CTRL[FULL] (full-cycle reload enabled), RM
 * 54.5.x reg summary.
 */
#define PWM_CTRL_RESET     0x0400u

/* SM_DMAEN bits. */
/* Value Registers DMA Enable (PWM_DMAEN_VALDE_MASK) */
#define PWM_DMAEN_VALDE    0x0200u
/* Capture A0 DMA Enable (PWM_DMAEN_CA0DE_MASK) */
#define PWM_DMAEN_CA0DE    0x0010u

/* SM_CAPTCTRLA (CMSIS PWM_CAPTCTRLA_*). */
#define PWM_CAPTCTRLA_ARMA    0x0001u  /* input A capture armed        */
/* edge-select for capture circuit 0 */
#define PWM_CAPTCTRLA_EDGA0_MASK  0x000Cu
#define PWM_CAPTCTRLA_EDGA0_SHIFT 2
#define EDGA0_DISABLED 0u
#define EDGA0_FALLING  1u
#define EDGA0_RISING   2u
#define EDGA0_ANY      3u

/* STS / INTEN interrupt bits (submodule). */
#define PWM_STS_CMPF       0x003Fu /* compare flags */
/* input-A capture-0 flag (PWM_STS_CFA0_MASK) */
#define PWM_STS_CFA0       0x0400u
#define PWM_STS_RF         0x1000u /* reload flag */
#define PWM_INTEN_CMPIE    0x003Fu /* compare interrupt enables */
#define PWM_INTEN_CA0IE    0x0400u /* input-A capture-0 interrupt enable */
#define PWM_INTEN_RIE      0x1000u /* reload interrupt enable */

/* MCTRL.RUN bit for submodule 0 (bits [11:8], one per submodule). */
#define PWM_MCTRL_RUN_SM0  0x0100u

/*
 * SM_CTRL[PRSC]: counter prescaler, divide by 2^PRSC (CMSIS
 * PWM_CTRL_PRSC_*).
 */
#define PWM_CTRL_PRSC_MASK   0x0070u
#define PWM_CTRL_PRSC_SHIFT  4

/*
 * The submodule counter clock.
 *
 * ⚠ THIS USED TO BE AN INVENTED 10 ns/tick ("nominal ~100 MHz") — a number
 * that appears nowhere in the RM and is not derived from the modelled clock
 * tree.  It survived because THE TEST DIVIDED BY THE SAME CONSTANT: the
 * carrier period was "verified" against SysTick, which independently
 * measures ELAPSED TIME and so genuinely catches a DRIFTING timer (that is
 * what it was written for) — but it CANNOT catch a wrong TICK RATE, because
 * the prediction and the model are wrong together and SysTick cheerfully
 * confirms them.  A careful measurement on one side of the comparison made
 * the other side invisible.  (backend hit the exact same shape in a perf/W
 * ratio: "THE STALE TERM IS WHICHEVER ONE YOU DID NOT JUST WORK ON.")
 *
 * Per the RM the counter is clocked from the IPBus clock
 * (CTRL2[CLK_SEL]=0), divided by 2^CTRL[PRSC].  The clock tree is not
 * modelled, so the IPBus rate is a DOCUMENTED ASSUMPTION tied to the SoC's
 * system clock (mcxn_frdm.c drives sysclk at 150 MHz) rather than a
 * free-floating constant.  The PRESCALER and the INIT/VAL1 relationship are
 * EXACT; only the base rate is an assumption, and it is now a traceable
 * one.
 */
/*
 * ⭐ VERIFIED AGAINST THE GUEST, NOT ASSUMED -- AND IT IS CORRECT.
 *
 * The SCT next door was ticking 3.1x too fast because it had a SELECTOR
 * (SYSCON[SCTCLKSEL]) that the model ignored, and its test shared the
 * model's assumption, so both were wrong together and the test passed.  I
 * went looking for the same bug here.  IT IS NOT HERE, and saying so
 * plainly is worth as much as a fix:
 *
 *   * fsl_pwm's source is CLOCK_GetFreq(kCLOCK_BusClk), and the SDK
 *     resolves that to CLOCK_GetCoreSysClkFreq() -- THE FlexPWM HAS NO
 *     SELECTOR OF ITS OWN.  It runs on the core/bus clock, which is also
 *     what SysTick counts.
 *   * I broke the stock pwm example at PWM_SetupPwm under gdb and read the
 *     argument the GUEST computed from the registers IT had programmed:
 *
 *         srcClock_Hz = 150000000
 *
 *     which is exactly what this model ticks at.  They agree BECAUSE BOTH
 *     ARE RIGHT, not because both are fabricated.  (And tests/mcxn-pwm's
 *     golden -- SysTick ticks == (VAL1+1) << PRSC -- rests on "PWM clock ==
 *     SysTick clock", which is a genuine silicon fact here, not a shared
 *     assumption smuggled in from the model.)
 *
 * ⚠ BUT IT IS CORRECT BY CONFIGURATION, NOT BY DERIVATION, AND THAT IS A
 * NAMED GAP.
 *
 * This is a CONSTANT.  It does not FOLLOW.  It is right because
 * BOARD_InitBootClocks happens to leave the core at 150 MHz; firmware that
 * reconfigures the core clock would have its SDK compute a different
 * srcClock_Hz while this model kept ticking at 150 MHz.  Closing that means
 * modelling the CORE clock itself as derived from SCG -- which also feeds
 * SysTick and the CPU -- and that is the last unbuilt piece of the tree.
 *
 *     ⭐ A RESULT THAT IS CORRECT BY LUCK IS A RESULT YOU HAVE NOT CHECKED.
 *        I checked.  It is correct by CONFIGURATION, which is luck with a
 *        name -- so the gap is recorded here as a decision, not discharged
 *        by a flag.
 */
/*
 * ⚠ SOURCED, NOT MEASURED — AND LAW 1 SAYS A CITATION IS A HYPOTHESIS, NOT
 * A RESULT.
 *
 *   150 MHz is the board's sysclk (mcxn_frdm.c), which is itself a
 *   documented ASSUMPTION: the clock tree is not modelled.  I once recorded
 *   this constant as "VERIFIED CORRECT via gdb -- the guest passes
 *   srcClock_Hz = 150000000".
 *
 *     ⭐ THAT VERIFIED THE SDK'S ASSUMPTION AGAINST MY ASSUMPTION.  Two
 *       documents agreeing is not a measurement; it is a CONSENSUS OF
 *       CITATIONS.
 *
 *   ✅ What IS measured: the PWM's carrier scales EXACTLY with the
 *      prescaler.  tests/mcxn-pwm sweeps PRSC = 0/1/3 and measures 32780 /
 *      65555 / 262210 SysTick ticks against a predicted (VAL1+1) << PRSC --
 *      and the prediction is in TICKS, in which this constant cancels.
 *      (Note they are 32780 and not 32776: MEASURED numbers are never
 *      exactly round.  An exact ratio is the fingerprint of
 *      multiplication.)
 *
 *   ⚠ What is NOT: the carrier's frequency in Hz.  That inherits this
 *      assumption whole.
 */
/*
 * ✅ RESOLVED (2026-07-20): the IPBus rate is no longer an assumed constant.
 * The FlexPWM counter is clocked by the bus clock (CTRL2[CLK_SEL]=0), which
 * is the SCG main clock; the SoC now drives a real Clock input, so
 * `mcxn_pwm_period_ns` reads clock_get_hz(s->clk) -- 48 MHz at the FRO_HF
 * reset, 150 MHz once firmware brings up PLL0, and it FOLLOWS a
 * reconfigure.  The old `PWM_IPBUS_HZ 150000000` constant (the "documented
 * assumption" the comments above lament) is gone; the prescaler ratio was
 * always exact, and now the absolute carrier frequency is too.
 */

/* Top-level (shared) registers. */
#define PWM_OUTEN          0x180
#define PWM_MASK           0x182
#define PWM_SWCOUT         0x184
#define PWM_DTSRCSEL       0x186
#define PWM_MCTRL          0x188   /* Master Control */
#define PWM_MCTRL2         0x18A
#define PWM_FCTRL          0x18C
#define PWM_FSTS           0x18E   /* Fault Status (W1C flags) */
#define PWM_FFILT          0x190
#define PWM_FTST           0x192
#define PWM_FCTRL2         0x194

/* MCTRL fields. */
#define PWM_MCTRL_LDOK_MASK    0x000Fu
#define PWM_MCTRL_CLDOK_MASK   0x00F0u
#define PWM_MCTRL_RUN_MASK     0x0F00u

/* STS W1C flag mask (CMPF/CFX/CFB/CFA/RF/REF/RUF). */
#define PWM_STS_W1C_MASK       0x7FFFu
/* FSTS lower W1C fault flags (FFLAG[3:0]). */
#define PWM_FSTS_W1C_MASK      0x000Fu

/*
 * Fault control/status fields (CMSIS PWM_FCTRL / PWM_FSTS), per-fault bit n.
 * FIE/FFLAG occupy bits[3:0]; FAUTO/FFPIN bits[11:8]; FLVL bits[15:12].
 */
#define PWM_FCTRL_FIE_MASK     0x000Fu   /* Fault Interrupt Enable [3:0]   */
#define PWM_FCTRL_FAUTO_MASK   0x0F00u   /* Automatic Fault Clearing [11:8] */
/* Fault Level (active level) [15:12] */
#define PWM_FCTRL_FLVL_MASK    0xF000u
#define PWM_FSTS_FFLAG_MASK    0x000Fu   /* Fault Flags (latched, W1C) [3:0] */
/* Filtered Fault Pins (live, RO) [11:8] */
#define PWM_FSTS_FFPIN_MASK    0x0F00u
/* FAULT0 single-bit selectors (the operator-driven input). */
#define PWM_FCTRL_FIE0         0x0001u
#define PWM_FCTRL_FAUTO0       0x0100u
#define PWM_FCTRL_FLVL0        0x1000u
#define PWM_FSTS_FFLAG0        0x0001u
#define PWM_FSTS_FFPIN0        0x0100u

static inline uint32_t pwm_ld16(MCXNPWMState *s, hwaddr off)
{
    return s->regs[off] | ((uint32_t)s->regs[off + 1] << 8);
}

static inline void pwm_st16(MCXNPWMState *s, hwaddr off, uint16_t v)
{
    s->regs[off] = v & 0xff;
    s->regs[off + 1] = (v >> 8) & 0xff;
}

static int64_t mcxn_pwm_period_ns(MCXNPWMState *s);   /* defined below */

/*
 * Submodule-0 reload/compare interrupt: (STS & INTEN) on the IRQ-bearing
 * bits.
 */
static void mcxn_pwm_update_irq(MCXNPWMState *s)
{
    uint16_t sts = pwm_ld16(s, PWM_SM_STS);
    uint16_t inten = pwm_ld16(s, PWM_SM_INTEN);
    bool active = (sts & inten &
                   (PWM_STS_RF | PWM_STS_CMPF | PWM_STS_CFA0)) != 0;

    qemu_set_irq(s->irq, active);
}

/*
 * The submodule-0 counter's live position at virtual time `now`.
 *
 * The model runs the counter as a periodic reload timer (next_reload_ns is
 * the deadline of the next INIT->VAL1 wrap); it does not store a
 * tick-by-tick count.  A capture reads the counter AT the input edge, so
 * reconstruct it from the timing: the fraction of the current period
 * already elapsed, mapped onto [INIT, VAL1].  When the counter is not
 * running the position is INIT (its reset value).
 */
static uint16_t mcxn_pwm_counter_now(MCXNPWMState *s, int64_t now)
{
    uint16_t init = pwm_ld16(s, PWM_SM_INIT);
    uint16_t val1 = pwm_ld16(s, PWM_SM_VAL1);
    int64_t period = mcxn_pwm_period_ns(s);
    int64_t remaining, elapsed, span, pos;

    if (period <= 0 || !timer_pending(&s->reload_timer)) {
        return init;
    }
    remaining = s->next_reload_ns - now;
    if (remaining < 0) {
        remaining = 0;
    } else if (remaining > period) {
        remaining = period;
    }
    /* ns since the counter was at INIT */
    elapsed = period - remaining;
    span = (uint16_t)(val1 - init) + 1;            /* INIT..VAL1 inclusive */
    pos = init + (elapsed * span) / period;        /* linear over the period */
    if (pos > val1) {
        pos = val1;
    }
    return (uint16_t)pos;
}

/*
 * Submodule-0 value-register DMA request (reload-driven).
 *
 * A DMA-driven FlexPWM control loop (PWM_SetupPwmDMA / the fsl_pwm value-DMA
 * path) arms an eDMA channel at the VALx registers, sets SM0.DMAEN[VALDE],
 * and then never writes the duty-cycle words itself: on every RELOAD the
 * FlexPWM raises the value DMA request (mux source FlexPWM0 Val0 = 43), the
 * eDMA writes the next VALx word(s), and the new duty cycle takes effect at
 * the following reload.  Before this line was wired, VALDE drove nothing --
 * the request could never assert, and a value-DMA loop that armed a channel
 * and waited for the reload request waited forever.
 *
 * The request is EDGE-driven by the reload event, not a FIFO level: one
 * reload must produce exactly one eDMA minor loop.  We assert here on
 * the reload and DEASSERT when the eDMA's write lands on a VALx register
 * (mcxn_pwm_write), which is the "consumption" that terminates the eDMA
 * service loop after a single minor loop -- exactly as a SAI TDR write
 * drops the SAI FIFO request.
 */
static void mcxn_pwm_set_val_dma(MCXNPWMState *s, bool level)
{
    if (level == s->val_dma_lvl) {
        return;
    }
    s->val_dma_lvl = level;
    qemu_set_irq(s->dma_req_val, level);
}

/*
 * Operator-driven CAPTURE on the submodule-0 input-A pin (capture circuit 0).
 *
 * A PWM input pin has no signal source in emulation, so the level is
 * OPERATOR-DRIVEN via the "capture-a-input" QOM property (the same seam
 * as PINT's pin-input / the CMP output).  A transition that matches
 * CAPTCTRLA[EDGA0] (falling / rising / any), while the input is armed
 * (CAPTCTRLA[ARMA]), latches the live counter position into CVAL0, sets
 * STS[CFA0], raises the capture interrupt if INTEN[CA0IE] is set, and --
 * if DMAEN[CA0DE] is set -- pulses the capture eDMA request (CMSIS FlexPWM0
 * capture0 = 39).  A capture is a one-shot event, so the DMA request goes
 * through the eDMA edge/pulse path (one edge, one minor loop).
 *
 * ⚠ Scope, stated: only input A, capture circuit 0 (CVAL0), of submodule
 * 0 is modelled.  Input B/X, capture circuit 1, the capture FIFO watermark
 * (CFAWM) and the edge counter are not -- an honest subset, the single-shot
 * capture the DMA path actually needs.
 */
static void mcxn_pwm_capture_a_edge(MCXNPWMState *s, bool level)
{
    uint16_t capctrl = pwm_ld16(s, PWM_SM_CAPTCTRLA);
    unsigned edgsel = (capctrl & PWM_CAPTCTRLA_EDGA0_MASK) >>
                      PWM_CAPTCTRLA_EDGA0_SHIFT;
    bool rising = level && !s->capa_level;
    bool falling = !level && s->capa_level;
    bool match;

    s->capa_level = level;

    if (!(capctrl & PWM_CAPTCTRLA_ARMA) || edgsel == EDGA0_DISABLED) {
        return;             /* not armed / capture disabled: ignore */
    }
    match = (edgsel == EDGA0_ANY) ||
            (edgsel == EDGA0_RISING && rising) ||
            (edgsel == EDGA0_FALLING && falling);
    if (!match) {
        return;                          /* wrong edge for the selected mode */
    }

    /*
     * Latch the live counter into CVAL0 (read-only to the guest) and flag
     * it.
     */
    pwm_st16(s, PWM_SM_CVAL0,
             mcxn_pwm_counter_now(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)));
    pwm_st16(s, PWM_SM_STS, pwm_ld16(s, PWM_SM_STS) | PWM_STS_CFA0);
    mcxn_pwm_update_irq(s);

    if (pwm_ld16(s, PWM_SM_DMAEN) & PWM_DMAEN_CA0DE) {
        qemu_irq_pulse(s->dma_req_capa);
    }
}

static void mcxn_pwm_set_capa(Object *obj, bool value, Error **errp)
{
    mcxn_pwm_capture_a_edge(MCXN_PWM(obj), value);
}

static bool mcxn_pwm_get_capa(Object *obj, Error **errp)
{
    return MCXN_PWM(obj)->capa_level;
}

/*
 * FlexPWM FAULT interrupt: any latched fault flag whose FIE is set asserts
 * FLEXPWMn_FAULT.  FFLAG and FIE are both bits[3:0], so they align directly.
 */
static void mcxn_pwm_update_fault_irq(MCXNPWMState *s)
{
    uint16_t fsts = pwm_ld16(s, PWM_FSTS);
    uint16_t fctrl = pwm_ld16(s, PWM_FCTRL);
    bool active = (fsts & PWM_FSTS_FFLAG_MASK & fctrl) != 0;

    qemu_set_irq(s->irq_fault, active);
}

/*
 * FlexPWM fault protection -- the safety path that trips motor gate-drive.
 *
 * A FAULT input pin has no signal source in emulation, so FAULT0's level is
 * OPERATOR-DRIVEN via the "fault-input" QOM property (the same seam as the
 * capture-a-input pin / PINT's pin-input / the CMP output).  Per the RM:
 *   - FCTRL[FLVL] picks the ACTIVE level: a fault condition exists when the
 *     (filtered) fault input equals FLVL for that fault.
 *   - FSTS[FFPIN] is a read-only mirror of the live filtered fault input;
 *     FSTS[FFLAG] LATCHES on the fault condition and stays set until cleared.
 *   - FCTRL[FIE] gates the FlexPWM FAULT interrupt (FLEXPWMn_FAULT to the
 *     NVIC).
 *   - Clearing FFLAG (W1C) succeeds only once the fault input has returned to
 *     normal -- a LIVE fault CANNOT be cleared (the real safety interlock);
 *     with FCTRL[FAUTO] set the flag clears automatically on return-to-normal.
 *
 * ⚠ Scope, stated: FAULT input 0 is the operator seam (faults 1..3 share the
 * identical per-bit logic but have no injection seam).  The fault's PRIMARY
 * silicon effect -- FORCING THE MAPPED PWM OUTPUTS to their safe state
 * (FCTRL[FSAFE]/DISMAP) -- is NOT modelled, because this model has no
 * output-pin / waveform representation at all (OUTEN/MASK are inert bytes).
 * What IS modelled is the fault DETECTION + STATUS + INTERRUPT path a
 * driver's fault handler binds to; the output-force-off is a named seam
 * awaiting an output-waveform model.
 */
static void mcxn_pwm_fault_eval(MCXNPWMState *s)
{
    uint16_t fctrl = pwm_ld16(s, PWM_FCTRL);
    uint16_t fsts = pwm_ld16(s, PWM_FSTS);
    bool flvl0 = (fctrl & PWM_FCTRL_FLVL0) != 0;
    /* input matches active level */
    bool active = (s->fault_level == flvl0);

    /* FFPIN[0]: read-only mirror of the live (filtered) fault-input level. */
    if (s->fault_level) {
        fsts |= PWM_FSTS_FFPIN0;
    } else {
        fsts &= ~PWM_FSTS_FFPIN0;
    }

    if (active) {
        fsts |= PWM_FSTS_FFLAG0;                  /* latch the fault */
    } else if (fctrl & PWM_FCTRL_FAUTO0) {
        /* automatic clear on return-to-normal */
        fsts &= ~PWM_FSTS_FFLAG0;
    }
    /* manual mode: stays latched until W1C */

    pwm_st16(s, PWM_FSTS, fsts);
    mcxn_pwm_update_fault_irq(s);
}

static void mcxn_pwm_set_fault(Object *obj, bool value, Error **errp)
{
    MCXNPWMState *s = MCXN_PWM(obj);

    s->fault_level = value;
    mcxn_pwm_fault_eval(s);
}

static bool mcxn_pwm_get_fault(Object *obj, Error **errp)
{
    return MCXN_PWM(obj)->fault_level;
}

/*
 * The submodule-0 PWM_A / PWM_B output levels -- the actual generated
 * waveform, WITH DEAD-TIME.
 *
 * PWM_A's output flip-flop is SET at VAL2 and RESET at VAL3, so its
 * commanded high region is [VAL2, VAL3); the duty cycle
 * (VAL3-VAL2)/(VAL1-INIT+1) is the number motor control is built on.
 * CTRL2[INDEP] chooses how PWM_B is generated:
 *
 *   INDEP=1 (independent): PWM_B has its own compare, [VAL4, VAL5); no
 *     dead-time.
 *
 *   INDEP=0 (complementary, the RESET default): PWM_B is the complement
 *     of PWM_A, and DEAD-TIME is inserted -- PWM_A's leading (0->1) edge at
 *     VAL2 is delayed by DTCNT0, PWM_B's leading edge at VAL3 by DTCNT1.
 *     During each delay BOTH outputs are forced inactive: the DEAD BAND
 *     that stops shoot-through through both transistors of an inverter leg.
 *     Without it (DTCNT=0) the pair overlap and the DC bus is shorted --
 *     which is exactly why DTCNT resets to 0x07FF (2047 cycles), not 0.
 *     DTCNT is in IPBus cycles regardless of PRSC, so in counter units it
 *     is DTCNT >> PRSC.
 *
 * PWM_X (the auxiliary output) is generated independently of the A/B mode:
 * set at VAL0, reset at VAL1 (high in (VAL0, VAL1)), the RM taking effect
 * at VAL0+1.  OCTRL[POLA/POLB/POLX] invert each output; OUTEN gates whether
 * the PWM drives the pin; a latched FAULT that DISMAP maps to an output
 * forces it inactive (the safety gate-drive cut).  The pins have no on-chip
 * consumer in emulation, so the levels are OPERATOR-OBSERVABLE via
 * read-only "pwm-a/b/x-output" QOM properties (a scope on the pins).  The
 * counter position is reconstructed from timing; when the submodule is not
 * running it is INIT, making the whole thing -- duty, complement, dead
 * band, PWM_X -- deterministically testable without racing the carrier.
 *
 * ⚠ Scope, stated: submodule 0; the complementary dead-time path assumes
 * VAL2 < VAL3 (the normal edge/center-aligned config).  FRACVAL
 * fractional-delay dead-time is deliberately NOT modelled: it is not a
 * static sub-count offset but a CROSS-CYCLE DITHER -- FRCTRL[FRACx_EN]
 * makes the accumulated fractional value grow every PWM cycle and add a
 * WHOLE count only every 1/frac cycles (RM 54.3.4.4: "every 4 PWM cycles
 * will be 1 clock cycle longer").  Representing it faithfully needs a
 * running fractional accumulator on the reload path, and observing it needs
 * edge-timing averaged over many cycles -- neither fits this
 * static-position model, and a fixed fractional offset would MISREPRESENT
 * the dither.  Left as an honest seam rather than a plausible fake.
 */
static void mcxn_pwm_outputs(MCXNPWMState *s, bool *a_out, bool *b_out,
                              bool *x_out)
{
    uint16_t fsts = pwm_ld16(s, PWM_FSTS);
    uint16_t dismap = pwm_ld16(s, PWM_SM_DISMAP0);
    uint16_t octrl = pwm_ld16(s, PWM_SM_OCTRL);
    uint16_t outen = pwm_ld16(s, PWM_OUTEN);
    uint16_t ctrl2 = pwm_ld16(s, PWM_SM_CTRL2);
    uint16_t ctrl  = pwm_ld16(s, PWM_SM_CTRL);
    unsigned prsc  = (ctrl & PWM_CTRL_PRSC_MASK) >> PWM_CTRL_PRSC_SHIFT;
    uint16_t val0 = pwm_ld16(s, PWM_SM_VAL0);
    uint16_t val1 = pwm_ld16(s, PWM_SM_VAL1);
    uint16_t val2 = pwm_ld16(s, PWM_SM_VAL2);
    uint16_t val3 = pwm_ld16(s, PWM_SM_VAL3);
    uint16_t pos = mcxn_pwm_counter_now(s,
                                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    bool a, b, x;

    if (ctrl2 & PWM_CTRL2_INDEP) {
        /*
         * Independent channels: A from VAL2/VAL3, B from VAL4/VAL5, no
         * dead-time.
         */
        uint16_t val4 = pwm_ld16(s, PWM_SM_VAL4);
        uint16_t val5 = pwm_ld16(s, PWM_SM_VAL5);
        a = (val2 <= val3) ? (pos >= val2 && pos < val3)
                            : (pos >= val2 || pos < val3);
        b = (val4 <= val5) ? (pos >= val4 && pos < val5)
                            : (pos >= val4 || pos < val5);
    } else {
        /* Complementary pair with dead-time inserted on each leading edge. */
        /* PWM_A 0->1 delay, in counts */
        uint16_t dead0 = pwm_ld16(s, PWM_SM_DTCNT0) >> prsc;
        /* PWM_B 0->1 delay, in counts */
        uint16_t dead1 = pwm_ld16(s, PWM_SM_DTCNT1) >> prsc;
        a = (pos >= (uint16_t)(val2 + dead0)) && (pos < val3);
        b = (pos < val2) || (pos >= (uint16_t)(val3 + dead1));
    }

    /*
     * PWM_X: auxiliary output, set at VAL0 (effective at VAL0+1), reset at
     * VAL1.
     */
    x = (pos > val0) && (pos < val1);

    if (octrl & PWM_OCTRL_POLA) {
        a = !a;
    }
    if (octrl & PWM_OCTRL_POLB) {
        b = !b;
    }
    if (octrl & PWM_OCTRL_POLX) {
        x = !x;
    }
    if (!(outen & PWM_OUTEN_PWMA_SM0)) {
        a = false;
    }
    if (!(outen & PWM_OUTEN_PWMB_SM0)) {
        b = false;
    }
    if (!(outen & PWM_OUTEN_PWMX_SM0)) {
        x = false;
    }
    /* FAULT force-off (safety) takes precedence */
    if (fsts & PWM_FSTS_FFLAG0) {
        if (dismap & PWM_DISMAP_DIS0A_F0) {
            a = false;
        }
        if (dismap & PWM_DISMAP_DIS0B_F0) {
            b = false;
        }
        if (dismap & PWM_DISMAP_DIS0X_F0) {
            x = false;
        }
    }
    *a_out = a;
    *b_out = b;
    *x_out = x;
}

static bool mcxn_pwm_get_a_output(Object *obj, Error **errp)
{
    bool a, b, x;
    mcxn_pwm_outputs(MCXN_PWM(obj), &a, &b, &x);
    return a;
}

static bool mcxn_pwm_get_b_output(Object *obj, Error **errp)
{
    bool a, b, x;
    mcxn_pwm_outputs(MCXN_PWM(obj), &a, &b, &x);
    return b;
}

static bool mcxn_pwm_get_x_output(Object *obj, Error **errp)
{
    bool a, b, x;
    mcxn_pwm_outputs(MCXN_PWM(obj), &a, &b, &x);
    return x;
}

/*
 * Submodule-0 counter period from INIT/VAL1, the CTRL[PRSC] prescaler and the
 * IPBus rate.
 *
 * ⚠ CTRL[PRSC] WAS NOT MODELLED AT ALL.  It is a 3-bit field selecting a
 * divide of 2^PRSC (1..128), and the model ignored it completely: a driver
 * that set PRSC=3 expecting a carrier EIGHT TIMES SLOWER got exactly the
 * same frequency.  That is a silent wrong answer in the one number motor
 * control is built on, and it is off by up to 128x.  The test never set
 * PRSC either — it exercised the single configuration in which the bug is
 * invisible (the same degenerate-shape failure as testing a matrix engine
 * only on square matrices).
 */
static int64_t mcxn_pwm_period_ns(MCXNPWMState *s)
{
    uint16_t init = pwm_ld16(s, PWM_SM_INIT);
    uint16_t val1 = pwm_ld16(s, PWM_SM_VAL1);
    uint16_t ctrl = pwm_ld16(s, PWM_SM_CTRL);
    unsigned prsc = (ctrl & PWM_CTRL_PRSC_MASK) >> PWM_CTRL_PRSC_SHIFT;
    int64_t span = (uint16_t)(val1 - init) + 1;   /* counter range, wraps ok */
    int64_t ticks = span << prsc;      /* 2^PRSC IPBus clocks/count */
    uint32_t hz = s->clk ? clock_get_hz(s->clk) : 0;
    int64_t ns;

    if (hz == 0) {
        return 0;   /* no counter clock (bus clock = 0): the PWM does not run */
    }
    ns = muldiv64(ticks, NANOSECONDS_PER_SECOND, hz);
    return ns < 1000 ? 1000 : ns;                 /* floor to keep it sane */
}

/*
 * FlexPWM submodule-0 OUTPUT TRIGGERS (PWM_OUT_TRIG0/1) -> INPUTMUX -> ADC.
 *
 * The motor-control SYNCHRONOUS-SAMPLING path: TCTRL[OUT_TRIG_EN] bit n
 * makes the VALn compare emit a trigger pulse -- even n -> OUT_TRIG0, odd
 * n -> OUT_TRIG1 (RM / CMSIS PWM_TCTRL_OUT_TRIG_EN) -- so the ADC samples
 * motor phase-current at a PRECISE point in the PWM carrier (typically
 * mid-cycle, away from the switching edges) with zero CPU involvement.
 * Unlike the value-register DMA request, which fires at the RELOAD boundary
 * and can ride the reload timer, an output trigger fires MID-PERIOD at the
 * VALn count, so it needs its own compare deadline: the reload timer only
 * knows the period edge.
 *
 * The counter sweeps [INIT, VAL1] once per period; it reaches value v at
 *   period_start + (v - INIT) * period / span,   span = VAL1 - INIT + 1
 * (period_start = next_reload_ns - period, i.e. the instant the counter
 * was last at INIT).
 *
 * ⚠ Scope, stated: submodule 0 only (the one submodule whose counter
 * this model runs) and the full-rate case (TCTRL[TRGFRQ]=0 -- a trigger
 * every period); TRGFRQ's every-other-period division is not modelled.
 * A VALn outside [INIT, VAL1] never matches (the counter reloads before
 * reaching it), exactly as on silicon.
 */
static int64_t mcxn_pwm_next_trig(MCXNPWMState *s, int64_t now,
                                   unsigned *mask_out)
{
    uint16_t tren = pwm_ld16(s, PWM_SM_TCTRL) & PWM_TCTRL_OUT_TRIG_EN_MASK;
    uint16_t init = pwm_ld16(s, PWM_SM_INIT);
    uint16_t val1 = pwm_ld16(s, PWM_SM_VAL1);
    int64_t period = mcxn_pwm_period_ns(s);
    int64_t span = (uint16_t)(val1 - init) + 1;
    int64_t period_start = s->next_reload_ns - period;
    int64_t best = 0;
    unsigned mask = 0;
    int n;

    *mask_out = 0;
    if (!tren || period <= 0 || span <= 0 || !timer_pending(&s->reload_timer)) {
        return 0;
    }
    for (n = 0; n < 6; n++) {
        uint16_t valn;
        int64_t off, t;
        unsigned line;

        if (!(tren & (1u << n))) {
            continue;
        }
        valn = pwm_ld16(s, PWM_SM_VAL0 + n * 4);
        if (valn < init || valn > val1) {
            continue;                    /* never reached in [INIT, VAL1] */
        }
        off = (uint16_t)(valn - init);
        t = period_start + off * period / span;
        while (t <= now) {
            /* this compare has passed: next period's copy */
            t += period;
        }
        /* even VALn -> OUT_TRIG0, odd -> OUT_TRIG1 */
        line = 1u << (n & 1);
        if (best == 0 || t < best) {
            best = t;
            mask = line;
        } else if (t == best) {
            mask |= line;                /* two VALn share this instant */
        }
    }
    *mask_out = mask;
    return best;
}

static void mcxn_pwm_arm_trig(MCXNPWMState *s, int64_t now)
{
    unsigned mask;
    int64_t t = mcxn_pwm_next_trig(s, now, &mask);

    if (t == 0) {
        timer_del(&s->trig_timer);
        s->trig_mask = 0;
        return;
    }
    s->next_trig_ns = t;
    s->trig_mask = (uint8_t)mask;
    timer_mod(&s->trig_timer, t);
}

static void mcxn_pwm_trig_tick(void *opaque)
{
    MCXNPWMState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->trig_mask & 1) {
        qemu_irq_pulse(s->out_trig[0]);
    }
    if (s->trig_mask & 2) {
        qemu_irq_pulse(s->out_trig[1]);
    }
    /* schedule the next enabled compare */
    mcxn_pwm_arm_trig(s, now);
}

/*
 * One submodule-0 reload: set the reload flag and re-arm.
 *
 * The next deadline is computed from the PREVIOUS DEADLINE, not from
 * "now". Re-arming from the current time re-adds the callback's dispatch
 * latency every single period, so the error ACCUMULATES and the carrier
 * runs systematically slow and drifts — a real PWM carrier does not.
 * Measuring the period against SysTick (an independent clock) showed the
 * old code running ~8.6% slow; counting interrupts, as this block's test
 * used to, could never have seen it.
 */
static void mcxn_pwm_reload_tick(void *opaque)
{
    MCXNPWMState *s = opaque;

    pwm_st16(s, PWM_SM_STS, pwm_ld16(s, PWM_SM_STS) | PWM_STS_RF);
    mcxn_pwm_update_irq(s);

    /*
     * On a reload, raise the value-register DMA request if the guest
     * enabled it.  The eDMA writes the next VALx word(s); that write
     * deasserts it again.
     */
    if (pwm_ld16(s, PWM_SM_DMAEN) & PWM_DMAEN_VALDE) {
        mcxn_pwm_set_val_dma(s, true);
    }

    {
        int64_t period = mcxn_pwm_period_ns(s);

        if (period <= 0) {
            /* no counter clock -- stop rather than spin at zero period */
            return;
        }
        s->next_reload_ns += period;
        timer_mod(&s->reload_timer, s->next_reload_ns);
        /*
         * Re-frame the output triggers onto the new period (period_start
         * moved).
         */
        mcxn_pwm_arm_trig(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static uint64_t mcxn_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNPWMState *s = MCXN_PWM(opaque);
    uint64_t val = 0;
    unsigned i;

    /* Assemble the requested width from the byte-addressable backing. */
    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_PWM_SIZE) {
            val |= (uint64_t)s->regs[offset + i] << (8 * i);
        }
    }
    return val;
}

static void mcxn_pwm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNPWMState *s = MCXN_PWM(opaque);
    unsigned i;
    int n;

    /*
     * Special-case the registers with side effects.  These are all 16-bit
     * and naturally addressed; for sub-register writes fall through to the
     * plain byte store below.
     */
    if (size == 2 && offset == PWM_MCTRL) {
        uint16_t mctrl = value & 0xffff;
        uint16_t run = mctrl & PWM_MCTRL_RUN_MASK;
        /*
         * RUN bits reflect back so firmware sees the PWM running.  LDOK is
         * a "load OK" request that hardware clears once the buffered
         * registers are loaded; model the load as instantaneous so LDOK
         * reads back 0.  CLDOK ("clear LDOK") forces the corresponding
         * LDOK bits to 0.
         */
        uint16_t cldok = (mctrl & PWM_MCTRL_CLDOK_MASK) >> 4;
        (void)cldok;   /* load is instantaneous, so LDOK is already cleared */
        pwm_st16(s, PWM_MCTRL, run);
        /*
         * Submodule-0 RUN gates the periodic reload timer.  Anchor the
         * first deadline here; every later one is derived from it, so the
         * carrier cannot drift (see mcxn_pwm_reload_tick).
         */
        if (run & PWM_MCTRL_RUN_SM0) {
            int64_t period = mcxn_pwm_period_ns(s);

            if (period > 0) {
                s->next_reload_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                     period;
                timer_mod(&s->reload_timer, s->next_reload_ns);
                mcxn_pwm_arm_trig(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            } else {
                /* no counter clock: does not run */
                timer_del(&s->reload_timer);
                timer_del(&s->trig_timer);
            }
        } else {
            timer_del(&s->reload_timer);
            timer_del(&s->trig_timer);
        }
        return;
    }

    if (size == 2 && offset < PWM_SM_BASE(PWM_SM_COUNT)) {
        /* Submodule status register: write-1-to-clear. */
        for (n = 0; n < PWM_SM_COUNT; n++) {
            hwaddr sts = PWM_SM_BASE(n) + PWM_SM_STS;
            if (offset == sts) {
                uint16_t cur = pwm_ld16(s, sts);
                cur &= ~((uint16_t)value & PWM_STS_W1C_MASK);
                pwm_st16(s, sts, cur);
                if (n == 0) {
                    mcxn_pwm_update_irq(s);
                }
                return;
            }
        }
    }

    if (size == 2 && offset == PWM_FSTS) {
        uint16_t cur = pwm_ld16(s, PWM_FSTS);
        uint16_t fctrl = pwm_ld16(s, PWM_FCTRL);
        uint16_t blocked = 0;
        uint16_t clr;

        /*
         * A LIVE fault cannot be cleared: if FAULT0 is still in its active
         * state (input == FLVL0), a W1C to FFLAG0 is refused -- the safety
         * interlock, the whole point of the latch.  (Faults 1..3 have no
         * injection seam, so they are never latched and clearing them is a
         * no-op regardless.)
         */
        if (s->fault_level == ((fctrl & PWM_FCTRL_FLVL0) != 0)) {
            blocked |= PWM_FSTS_FFLAG0;
        }
        /* FFPIN (RO) untouched */
        clr = (uint16_t)value & PWM_FSTS_W1C_MASK & ~blocked;
        cur &= ~clr;
        pwm_st16(s, PWM_FSTS, cur);
        mcxn_pwm_update_fault_irq(s);
        return;
    }

    /* Generic byte-addressable store. */
    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_PWM_SIZE) {
            s->regs[offset + i] = (value >> (8 * i)) & 0xff;
        }
    }

    /* A write touching submodule-0 INTEN can change the interrupt condition. */
    if (offset <= PWM_SM_INTEN + 1 && offset + size > PWM_SM_INTEN) {
        mcxn_pwm_update_irq(s);
    }

    /*
     * A write touching FCTRL changes FLVL/FAUTO/FIE: re-evaluate the fault
     * with the current input level (new FLVL may make a steady input a
     * fault; new FAUTO may auto-clear a latched flag; new FIE may
     * (un)gate the interrupt).
     */
    if (offset <= PWM_FCTRL + 1 && offset + size > PWM_FCTRL) {
        mcxn_pwm_fault_eval(s);
    }

    /*
     * A write landing on submodule-0's VALx block is the eDMA consuming
     * the reload-driven value request: deassert it so the eDMA service
     * loop stops after this single minor loop (see mcxn_pwm_set_val_dma).
     */
    if (s->val_dma_lvl && offset + size > PWM_SM_VAL0 &&
        offset <= PWM_SM_VAL5 + 1) {
        mcxn_pwm_set_val_dma(s, false);
    }

    /*
     * A write to submodule-0's counting/compare config (INIT, CTRL/PRSC,
     * VAL0..VAL5) or TCTRL[OUT_TRIG_EN] changes WHEN the output triggers
     * fire: re-frame them while the submodule is running.  (INIT..TCTRL is
     * one contiguous span, 0x02..0x2B.)
     */
    if (offset < PWM_SM_TCTRL + 2 && offset + size > PWM_SM_INIT &&
        timer_pending(&s->reload_timer)) {
        mcxn_pwm_arm_trig(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

static const MemoryRegionOps mcxn_pwm_ops = {
    .read = mcxn_pwm_read,
    .write = mcxn_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_pwm_reset(DeviceState *dev)
{
    MCXNPWMState *s = MCXN_PWM(dev);

    int n;

    memset(s->regs, 0, sizeof(s->regs));
    /*
     * RM reset values that are NOT zero -- and where zero is a legal,
     * meaningful, CATASTROPHIC value.  DTCNT0/1 reset to 0x07FF (max
     * dead-time): zero dead-time is a direct short across the DC bus
     * through both transistors of an inverter leg.  DISMAP0 resets to
     * 0xFFFF (every fault disables every output -- the safe default).
     * memset(0) shipped both dangerous zeros.
     */
    for (n = 0; n < PWM_SM_COUNT; n++) {
        hwaddr b = PWM_SM_BASE(n);
        pwm_st16(s, b + PWM_SM_CTRL, PWM_CTRL_RESET);
        pwm_st16(s, b + PWM_SM_DTCNT0, PWM_DTCNT_RESET);
        pwm_st16(s, b + PWM_SM_DTCNT1, PWM_DTCNT_RESET);
        pwm_st16(s, b + PWM_SM_DISMAP0, PWM_DISMAP_RESET);
    }
    timer_del(&s->reload_timer);
    timer_del(&s->trig_timer);
    s->trig_mask = 0;
    qemu_set_irq(s->irq, 0);
    s->val_dma_lvl = false;
    qemu_set_irq(s->dma_req_val, 0);
    s->capa_level = false;
    qemu_set_irq(s->dma_req_capa, 0);
    s->fault_level = false;
    qemu_set_irq(s->irq_fault, 0);
}

static void mcxn_pwm_init(Object *obj)
{
    MCXNPWMState *s = MCXN_PWM(obj);

    /* Bus/IPBus clock input — the SoC connects it to the SCG main clock. */
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);

    /*
     * Operator-driven submodule-0 input-A capture pin:
     *   qom-set /machine/soc/pwm0 capture-a-input true
     */
    object_property_add_bool(obj, "capture-a-input",
                             mcxn_pwm_get_capa, mcxn_pwm_set_capa);

    /*
     * Operator-driven FAULT0 input pin (the safety seam):
     *   qom-set /machine/soc/pwm0 fault-input true
     */
    object_property_add_bool(obj, "fault-input",
                             mcxn_pwm_get_fault, mcxn_pwm_set_fault);

    /*
     * Operator-OBSERVABLE submodule-0 PWM_A / PWM_B output levels
     * (read-only; the "scope").  In complementary mode the dead band
     * shows as BOTH reading false at once:
     *   qom-get /machine/soc/pwm0 pwm-a-output ; qom-get /machine/soc/pwm0
     *   pwm-b-output
     */
    object_property_add_bool(obj, "pwm-a-output",
                             mcxn_pwm_get_a_output, NULL);
    object_property_add_bool(obj, "pwm-b-output",
                             mcxn_pwm_get_b_output, NULL);
    object_property_add_bool(obj, "pwm-x-output",
                             mcxn_pwm_get_x_output, NULL);
}

static void mcxn_pwm_realize(DeviceState *dev, Error **errp)
{
    MCXNPWMState *s = MCXN_PWM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_pwm_ops, s,
                          TYPE_MCXN_PWM, MCXN_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    /* 0: NVIC reload/compare */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    /* 1: value-register DMA req */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req_val);
    /* 2: input-A capture DMA req */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req_capa);
    /* 3: FLEXPWMn_FAULT to NVIC */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq_fault);
    /*
     * Submodule-0 output triggers PWM_OUT_TRIG0/1 -> INPUTMUX -> ADC
     * hardware trigger.
     */
    qdev_init_gpio_out_named(dev, s->out_trig, "out-trig", 2);
    timer_init_ns(&s->reload_timer, QEMU_CLOCK_VIRTUAL, mcxn_pwm_reload_tick,
                  s);
    timer_init_ns(&s->trig_timer, QEMU_CLOCK_VIRTUAL, mcxn_pwm_trig_tick, s);
}

/* Re-drive both IRQ lines from restored register state after migration: the
 * output lines are not part of vmstate, so a VM migrated with STS & INTEN or a
 * fault flag asserted would otherwise land with the lines low. */
static int mcxn_pwm_post_load(void *opaque, int version_id)
{
    MCXNPWMState *s = MCXN_PWM(opaque);

    mcxn_pwm_update_irq(s);
    mcxn_pwm_update_fault_irq(s);
    return 0;
}

static const VMStateDescription vmstate_mcxn_pwm = {
    .name = TYPE_MCXN_PWM,
    .version_id = 5,
    .minimum_version_id = 5,
    .post_load = mcxn_pwm_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MCXNPWMState, MCXN_PWM_SIZE),
        VMSTATE_TIMER(reload_timer, MCXNPWMState),
        VMSTATE_INT64(next_reload_ns, MCXNPWMState),
        VMSTATE_BOOL(val_dma_lvl, MCXNPWMState),
        VMSTATE_BOOL(capa_level, MCXNPWMState),
        VMSTATE_BOOL(fault_level, MCXNPWMState),
        VMSTATE_TIMER(trig_timer, MCXNPWMState),
        VMSTATE_INT64(next_trig_ns, MCXNPWMState),
        VMSTATE_UINT8(trig_mask, MCXNPWMState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_pwm_realize;
    device_class_set_legacy_reset(dc, mcxn_pwm_reset);
    dc->vmsd = &vmstate_mcxn_pwm;
}

static const TypeInfo mcxn_pwm_types[] = {
    {
        .name          = TYPE_MCXN_PWM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPWMState),
        .instance_init = mcxn_pwm_init,
        .class_init    = mcxn_pwm_class_init,
    },
};

DEFINE_TYPES(mcxn_pwm_types)
