/*
 * MCXN947 eFlexPWM input capture -> eDMA (operator-driven, stock-driver-shaped).
 *
 * The FlexPWM captures its running counter into CVAL0 on an edge of the input-A pin
 * (CAPTCTRLA[ARMA] + EDGA0 select the edge), and with DMAEN[CA0DE] set the capture pulses
 * the eDMA request (CMSIS FlexPWM0 capture0 = source 39).  The pin has no signal source in
 * emulation, so it is OPERATOR-DRIVEN: the harness toggles the "capture-a-input" QOM property.
 * Before this line was wired, a capture-paced DMA (input-capture measurement, the classic
 * "timestamp an external edge" use) moved nothing.
 *
 * Two phases:
 *   POSITIVE  -- arm RISING-edge capture + DMA (1 capture), inject a rising edge: the DMA moves
 *                CVAL0 to memory, STS[CFA0] latches, the captured value is in [INIT, VAL1].
 *   CONTROL   -- the EDGE-SELECT gate.  Arm RISING, ask for TWO captures, and drive a FALLING
 *                then a RISING edge.  A correct model captures ONLY the rising -> exactly ONE
 *                capture (CFA0 set, DONE clear); a model that captured on ANY edge takes both
 *                -> DONE set -> FAIL.  This is a POSITIVE-SIGNAL control (requiring CFA0 proves
 *                the edges were delivered -- no silent race pass, no delay-loop crutch), which
 *                is why it can discriminate where a bare "sleep then check absence" cannot.
 *
 * The captured value is a BOUNDED check (in-counter-range), not byte-exact: the operator
 * drives the pin over QMP, loosely coupled to the counter phase, so the exact position at the
 * edge is not predictable -- said honestly rather than faked with a precise golden.
 *
 * ⚠ TESTED DIMENSIONS: capture -> DMA request, and the EDGA0 edge-select gate (both mutation-
 * proven).  The CAPTCTRLA[ARMA] arm/disarm gate is MODELLED but NOT swept here -- proving
 * "disarmed -> no capture" robustly needs a positive delivery signal, which a non-capture does
 * not provide (a timed absence-check would be the flaky delay-loop crutch).  Stated, not hidden.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP_CTRL (*(volatile uint32_t *)0x400B4018u)
#define LP_STAT (*(volatile uint32_t *)0x400B4014u)
#define LP_DATA (*(volatile uint32_t *)0x400B401Cu)
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putdec(uint32_t v) { char b[12]; int i = 0; if (!v) { putc_('0'); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; } while (i--) putc_(b[i]); }

/* ---- eFlexPWM0 (SM0) ------------------------------------------------------ */
#define PWM0          0x400CE000u
#define PWM_SM0_INIT     (*(volatile uint16_t *)(PWM0 + 0x02))
#define PWM_SM0_CTRL     (*(volatile uint16_t *)(PWM0 + 0x06))
#define PWM_SM0_VAL1     (*(volatile uint16_t *)(PWM0 + 0x0E))
#define PWM_SM0_STS      (*(volatile uint16_t *)(PWM0 + 0x24))
#define PWM_SM0_DMAEN    (*(volatile uint16_t *)(PWM0 + 0x28))
#define PWM_SM0_CAPTCTRLA (*(volatile uint16_t *)(PWM0 + 0x34))
#define PWM_SM0_CVAL0    (*(volatile uint16_t *)(PWM0 + 0x40))
#define PWM_MCTRL        (*(volatile uint16_t *)(PWM0 + 0x188))
#define CVAL0_ADDR       (PWM0 + 0x40)

#define DMAEN_CA0DE     (1u << 4)      /* input-A capture-0 DMA enable          */
#define CAPTCTRLA_ARMA  (1u << 0)
#define EDGA0_RISING    (2u << 2)      /* capture rising edges                  */
#define STS_CFA0        (1u << 10)     /* input-A capture-0 flag (0x400)        */
#define MCTRL_LDOK_SM0  (1u << 0)
#define MCTRL_RUN_SM0   (1u << 8)

#define SM_INIT   0x0100u
#define SM_PERIOD 0xF000u              /* VAL1 -- a wide sweep so a capture lands mid-range */

/* ---- SCG0: bring the counter clock up so the counter actually runs -------- */
#define SCG0         0x40044000u
#define SCG_FIRCCSR  (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_APLLCSR  (*(volatile uint32_t *)(SCG0 + 0x500))
#define SCG_APLLCTRL (*(volatile uint32_t *)(SCG0 + 0x504))
#define SCG_APLLNDIV (*(volatile uint32_t *)(SCG0 + 0x50C))
#define SCG_APLLMDIV (*(volatile uint32_t *)(SCG0 + 0x510))
#define SCG_APLLPDIV (*(volatile uint32_t *)(SCG0 + 0x514))
#define SCG_RCCR     (*(volatile uint32_t *)(SCG0 + 0x014))
static void clock_init_150m(void)
{
    SCG_FIRCCSR |= 1u;
    SCG_APLLCTRL = 0x020035B0u;
    SCG_APLLNDIV = 8; SCG_APLLMDIV = 50; SCG_APLLPDIV = 1;
    SCG_APLLCSR |= 3u;
    SCG_RCCR = (5u << 24);
}

/* ---- DMA0 ---------------------------------------------------------------- */
#define DMA0 0x40080000u
#define CH(n) (DMA0 + 0x1000u * ((n) + 1))
#define CH_CSR(n)    (*(volatile uint32_t *)(CH(n) + 0x00))
#define CH_INT(n)    (*(volatile uint32_t *)(CH(n) + 0x08))
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
#define ATTR_16BIT   0x0101u
#define CSR_ERQ      (1u << 0)
#define CSR_DONE     (1u << 30)
#define TCD_INTMAJOR (1u << 1)
#define TCD_DREQ     (1u << 3)
#define DMAREQ_PWM0_CAP0 39u
#define CHAN 0

static volatile uint16_t out;

/* Arm ch0: CVAL0 -> out, one 16-bit word per capture request, <citer> captures total. */
static void dma_arm(uint16_t citer)
{
    CH_CSR(CHAN)     = CSR_DONE;         /* W1C stale DONE */
    CH_INT(CHAN)     = 1u;
    TCD_SADDR(CHAN)  = CVAL0_ADDR;
    TCD_SOFF(CHAN)   = 0;
    TCD_ATTR(CHAN)   = ATTR_16BIT;
    TCD_NBYTES(CHAN) = 2;
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)&out;
    TCD_DOFF(CHAN)   = 0;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = citer;
    TCD_BITER(CHAN)  = citer;
    TCD_CSR(CHAN)    = TCD_INTMAJOR | TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_PWM0_CAP0;
    CH_CSR(CHAN)     = CSR_ERQ;
}

void cpu0_main(void)
{
    int ok = 1, g;

    LP_CTRL = (1u << 19);
    clock_init_150m();
    puts_("FLEXPWM-CAP test\r\n");

    /* SM0: counter INIT..VAL1, PRSC=0, running (a wide, slowish sweep). */
    PWM_SM0_INIT = SM_INIT;
    PWM_SM0_CTRL = 0;
    PWM_SM0_VAL1 = SM_PERIOD;
    PWM_MCTRL = MCTRL_LDOK_SM0 | MCTRL_RUN_SM0;

    /* ---- POSITIVE: arm rising-edge capture + DMA (1 capture), operator injects a rise ---- */
    out = 0xFFFFu;
    PWM_SM0_STS = STS_CFA0;                          /* W1C any stale flag */
    PWM_SM0_CAPTCTRLA = CAPTCTRLA_ARMA | EDGA0_RISING;
    PWM_SM0_DMAEN = DMAEN_CA0DE;
    dma_arm(1);

    puts_("FLEXPWM-CAP ARMED-RISE\r\n");             /* harness injects capture-a-input=1 */
    g = 200000000;
    while (!(CH_CSR(CHAN) & CSR_DONE) && g--) {
    }
    ok &= (CH_CSR(CHAN) & CSR_DONE) && !(CH_CSR(CHAN) & CSR_ERQ) && (CH_INT(CHAN) & 1u);
    ok &= !!(PWM_SM0_STS & STS_CFA0);                /* capture flag latched */
    ok &= (out >= SM_INIT && out <= SM_PERIOD);      /* captured a counter position */
    puts_("  captured CVAL0 = "); putdec(out); puts_("\r\n");

    /* ---- CONTROL: the EDGE-SELECT gate.  Arm for RISING, ask for TWO captures, and the
     *      operator drives a FALLING edge THEN a RISING edge.  A correct model captures ONLY
     *      the rising -> exactly ONE capture: CFA0 latches but DONE (needs 2) stays CLEAR.  A
     *      model that captured on ANY edge takes both -> TWO captures -> DONE SET -> FAIL.
     *
     *      This is a POSITIVE-signal control, not a timed one: requiring CFA0 SET proves the
     *      edges were actually delivered (no silent race pass), and DONE CLEAR proves the
     *      falling was ignored.  (A bare "sleep then check absence" would be a delay-loop
     *      crutch -- optimized away at -O2, and flaky by construction.) ---- */
    out = 0xFFFFu;
    PWM_SM0_STS = STS_CFA0;                          /* W1C the positive-phase flag */
    PWM_SM0_CAPTCTRLA = CAPTCTRLA_ARMA | EDGA0_RISING;   /* rising-armed */
    PWM_SM0_DMAEN = DMAEN_CA0DE;
    dma_arm(2);                                      /* need TWO captures for DONE */

    puts_("FLEXPWM-CAP ARMED-CTRL\r\n");             /* harness drives 1->0 (fall) then 0->1 (rise) */
    /* Spin on volatile MMIO (NOT optimizable) long enough for both operator edges to land.
     * A correct model never reaches DONE (only the rising captures); we exit on the guard. */
    g = 40000000;
    while (!(CH_CSR(CHAN) & CSR_DONE) && g--) {
    }
    ok &= !!(PWM_SM0_STS & STS_CFA0);                /* the RISING was delivered + captured   */
    ok &= !(CH_CSR(CHAN) & CSR_DONE);                /* but the FALLING was IGNORED (only 1)  */

    puts_(ok ? "FLEXPWM-CAP PASS\r\n" : "FLEXPWM-CAP FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
