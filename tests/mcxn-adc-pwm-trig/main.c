/*
 * MOTOR-CONTROL SYNCHRONOUS SAMPLING -- the FlexPWM triggers the ADC mid-carrier:
 *
 *     FlexPWM0 SM0, VAL0 compare  ->  PWM_OUT_TRIG0
 *                                 ->  INPUTMUX (selector 24 -> ADC0_TRIG[0])
 *                                 ->  ADC0 hardware trigger (TCTRL0[HTEN])
 *                                 ->  ADC FIFO watermark  ->  eDMA request 21
 *                                 ->  memory.
 *
 * This is how a field-oriented-control loop samples phase current: the ADC fires at a
 * PRECISE point in the PWM carrier (here VAL0, mid-period, away from the switching
 * edges) so the sample is taken at the current-ripple midpoint, every carrier cycle,
 * with ZERO CPU involvement.  The CPU never triggers a conversion and never reads
 * RESFIFO -- it configures the carrier + the ADC and the PWM paces everything.
 *
 * THE GAP THIS CLOSES.  The FlexPWM had no output-trigger path at all: TCTRL[OUT_TRIG_EN]
 * was a stored, inert bit and the submodule emitted no PWM_OUT_TRIGx, so
 * kINPUTMUX_Pwm0A0Trig0ToAdc0Trigger routed a signal that never pulsed.  A guest building
 * the canonical PWM-synchronised ADC loop got a carrier that ran and a trigger that never
 * fired -- silent, like the LPTMR/CTIMER trigger gaps before it.  It also needed real
 * MID-PERIOD timing: unlike the value-DMA request (reload boundary), an output trigger
 * fires at the VALn count inside the period, which the reload timer alone cannot express.
 *
 * Selector 24 = Pwm0A0Trig0 (FlexPWM0, submodule 0, OUT_TRIG0) is DERIVED from NXP's
 * compiled driver (kINPUTMUX_Pwm{m}A0Trig{t}ToAdc0 = 24 + 8*m + t), not guessed.
 *
 * TWO axes are asserted:
 *   DATA  -- every sample moved and carries RESFIFO[VALID] (the operator-set channel value).
 *   RATE  -- the 4 conversions span at least ~3 PWM carrier periods of SysTick, proving the
 *            trigger is PACED BY THE CARRIER (a compare mid-period), not fired instantly.
 *            PWM counter and SysTick share the derived bus clock, so a period is (VAL1+1)
 *            SysTick ticks (PRSC=0) -- the absolute rate cancels.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4 + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

static void putc_(char c)
{
    while (!(LP_STAT & STAT_TDRE)) {
    }
    LP_DATA = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s) {
        putc_(*s++);
    }
}

static void puthex(uint32_t v)
{
    const char *H = "0123456789ABCDEF";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        putc_(H[(v >> i) & 0xF]);
    }
}

/* ---- FlexPWM0 (CMSIS base 0x400CE000) ------------------------------------ */
#define PWM0 0x400CE000u
#define SM0_INIT  (*(volatile uint16_t *)(PWM0 + 0x02))
#define SM0_CTRL  (*(volatile uint16_t *)(PWM0 + 0x06))   /* PRSC */
#define SM0_VAL0  (*(volatile uint16_t *)(PWM0 + 0x0A))   /* OUT_TRIG0 compare */
#define SM0_VAL1  (*(volatile uint16_t *)(PWM0 + 0x0E))   /* modulo (period) */
#define SM0_VAL2  (*(volatile uint16_t *)(PWM0 + 0x12))   /* even -> OUT_TRIG0 (decoy) */
#define SM0_TCTRL (*(volatile uint16_t *)(PWM0 + 0x2A))   /* OUT_TRIG_EN */
#define PWM_MCTRL (*(volatile uint16_t *)(PWM0 + 0x188))
#define MCTRL_RUN_SM0    0x0100u
#define TCTRL_OT_EN_VAL0 0x0001u                          /* VAL0 -> OUT_TRIG0 */
#define PWM_VAL1 1200u
#define PWM_VAL0 600u                                     /* mid-period sample point */
#define PWM_VAL2 900u                                     /* DECOY: a second even-VAL
                                                           * compare, NON-zero (so it
                                                           * does not alias the reload
                                                           * instant) and NOT enabled in
                                                           * OUT_TRIG_EN -- a correct
                                                           * model never triggers on it.
                                                           * A model that ignores
                                                           * OUT_TRIG_EN fires here too ->
                                                           * ~2 conversions/period ->
                                                           * the RATE floor FAILS. */

/* ---- SysTick ------------------------------------------------------------- */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define SYST_ENABLE    (1u << 0)
#define SYST_CLKSOURCE (1u << 2)
#define SYST_MASK 0x00FFFFFFu

/* ---- ADC0 ---------------------------------------------------------------- */
#define ADC0 0x4010D000u
#define ADC_CTRL   (*(volatile uint32_t *)(ADC0 + 0x010))
#define ADC_DE     (*(volatile uint32_t *)(ADC0 + 0x01C))
#define ADC_FCTRL0 (*(volatile uint32_t *)(ADC0 + 0x0E0))
#define ADC_RESFIFO0_ADDR         (ADC0 + 0x300)
#define ADC_TCTRL0 (*(volatile uint32_t *)(ADC0 + 0x0A0))
#define ADC_CMDL0  (*(volatile uint32_t *)(ADC0 + 0x100))
#define ADC_CMDH0  (*(volatile uint32_t *)(ADC0 + 0x104))
#define CTRL_ADCEN   (1u << 0)
#define DE_FWMDE0    (1u << 0)
#define TCTRL_HTEN   (1u << 0)

/* ---- DMA0 ---------------------------------------------------------------- */
#define DMA0 0x40080000u
#define CH(n) (DMA0 + 0x1000u * ((n) + 1))
#define CH_CSR(n)    (*(volatile uint32_t *)(CH(n) + 0x00))
#define CH_MUX(n)    (*(volatile uint32_t *)(CH(n) + 0x14))
#define TCD_SADDR(n) (*(volatile uint32_t *)(CH(n) + 0x20))
#define TCD_SOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x24))
#define TCD_ATTR(n)  (*(volatile uint16_t *)(CH(n) + 0x26))
#define TCD_NBYTES(n)(*(volatile uint32_t *)(CH(n) + 0x28))
#define TCD_SLAST(n) (*(volatile uint32_t *)(CH(n) + 0x2C))
#define TCD_DADDR(n) (*(volatile uint32_t *)(CH(n) + 0x30))
#define TCD_DOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x34))
#define TCD_CITER(n) (*(volatile uint16_t *)(CH(n) + 0x36))
#define TCD_DLAST(n) (*(volatile uint32_t *)(CH(n) + 0x38))
#define TCD_CSR(n)   (*(volatile uint16_t *)(CH(n) + 0x3C))
#define TCD_BITER(n) (*(volatile uint16_t *)(CH(n) + 0x3E))
#define CSR_ERQ    (1u << 0)
#define CSR_DONE   (1u << 30)
#define TCD_DREQ   (1u << 3)
#define ATTR_32BIT ((2u << 8) | 2u)
#define DMAREQ_ADC0_FIFO_A 21
#define CHAN 3
#define NSAMP 4

static volatile uint32_t samples[NSAMP];

/* INPUTMUX0: ADC0_TRIG[4] @0x280. */
#define INPUTMUX0    0x40006000u
#define ADC0_TRIG0   (*(volatile uint32_t *)(INPUTMUX0 + 0x280))
#define TRIG_SRC_PWM0_SM0_TRIG0 24u     /* decoded from NXP's compiled driver */

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    int i;
    uint32_t t_before, t_after, elapsed, floor_ticks;

    LP_CTRL = CTRL_TE;
    puts_("ADC-PWM-TRIG test\r\n");

    for (i = 0; i < NSAMP; i++) {
        samples[i] = 0xEEEEEEEEu;            /* poison: a stub leaves this */
    }

    /* --- ADC: enabled, command 0 on channel 0, watermark 1 --------------- */
    ADC_CTRL   = CTRL_ADCEN;
    ADC_CMDL0  = 0;
    ADC_CMDH0  = 0;
    ADC_TCTRL0 = (1u << 24);                 /* trigger 0 -> command 1      */
    ADC_FCTRL0 = (1u << 16);                 /* FWMARK = 1                  */

    /* --- eDMA: ADC RESFIFO -> memory, two results per request ------------ */
    TCD_SADDR(CHAN)  = ADC_RESFIFO0_ADDR;
    TCD_SOFF(CHAN)   = 0;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 8;
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)samples;
    TCD_DOFF(CHAN)   = 4;
    TCD_DLAST(CHAN)  = (uint32_t)(-(int32_t)(4 * NSAMP));
    TCD_CITER(CHAN)  = NSAMP / 2;
    TCD_BITER(CHAN)  = NSAMP / 2;
    TCD_CSR(CHAN)    = TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_ADC0_FIFO_A;
    CH_CSR(CHAN)     = CSR_ERQ;
    ADC_DE           = DE_FWMDE0;

    /* Route FlexPWM0 SM0 OUT_TRIG0 -> ADC0_TRIG[0]. */
    ADC0_TRIG0 = TRIG_SRC_PWM0_SM0_TRIG0;

    /* --- FlexPWM0 SM0: carrier + VAL0 compare emits OUT_TRIG0 ------------- */
    SM0_INIT  = 0;
    SM0_VAL1  = PWM_VAL1;                     /* modulo -> carrier period    */
    SM0_VAL0  = PWM_VAL0;                     /* mid-period sample instant   */
    SM0_VAL2  = PWM_VAL2;                     /* decoy compare (NOT enabled) */
    SM0_CTRL  = 0;                            /* PRSC = 0 (/1)               */
    SM0_TCTRL = TCTRL_OT_EN_VAL0;             /* ONLY VAL0 compare -> OUT_TRIG0 */
    PWM_MCTRL = MCTRL_RUN_SM0;                /* GO                          */

    /* SysTick free-running on the (same, derived) bus clock, for the RATE check. */
    SYST_RVR = SYST_MASK;
    SYST_CVR = 0;
    SYST_CSR = SYST_ENABLE | SYST_CLKSOURCE;

    /*
     * PHASE 1 -- ROUTED BUT NOT ARMED.  TCTRL0[HTEN] is still CLEAR.  The carrier runs
     * and pulses OUT_TRIG0 every period, but an unenabled hardware trigger MUST NOT
     * convert.  Without this the test cannot tell a working HTEN gate from a model that
     * converts on any routed edge (more permissive than silicon).
     */
    for (d = 0; d < 400000; d++) {
    }
    for (i = 0; i < NSAMP; i++) {
        if (samples[i] != 0xEEEEEEEEu) {
            puts_("  UNARMED TRIGGER CONVERTED -- TCTRL[HTEN] is not gating\r\n");
            ok = 0;
            break;
        }
    }
    if (CH_CSR(CHAN) & CSR_DONE) {
        puts_("  UNARMED TRIGGER SET DONE\r\n");
        ok = 0;
    }
    puts_(ok ? "  phase1: routed but NOT armed -> no conversion (correct)\r\n"
             : "  phase1: FAILED\r\n");

    /* PHASE 2 -- ARM IT.  The carrier now paces the conversions, one per period. */
    t_before = SYST_CVR & SYST_MASK;
    ADC_TCTRL0 |= TCTRL_HTEN;

    for (d = 0; d < 20000000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }
    t_after = SYST_CVR & SYST_MASK;
    PWM_MCTRL = 0;                           /* stop the carrier            */

    /* SysTick counts DOWN; elapsed = before - after (no wrap: << 2^24). */
    elapsed = (t_before - t_after) & SYST_MASK;

    for (i = 0; i < NSAMP; i++) {
        puts_("  sample["); putc_((char)('0' + i)); puts_("] = ");
        puthex(samples[i]); puts_("\r\n");
        ok &= (samples[i] != 0xEEEEEEEEu);   /* DATA: it moved              */
        ok &= !!(samples[i] & (1u << 31));   /* DATA: RESFIFO[VALID]        */
    }
    ok &= !!(CH_CSR(CHAN) & CSR_DONE);

    /*
     * RATE: 4 conversions, one per carrier period, must span at least ~3 periods of
     * SysTick.  A model that fires the trigger instantly (ignoring the VAL0 compare
     * timing) finishes in ~0 ticks and FAILS this floor.
     */
    floor_ticks = 3u * (PWM_VAL1 + 1u);
    puts_("  elapsed ticks = "); puthex(elapsed);
    puts_("  floor = "); puthex(floor_ticks); puts_("\r\n");
    ok &= (elapsed >= floor_ticks);

    puts_(ok ? "PWM-TRIG PASS\r\n" : "PWM-TRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
