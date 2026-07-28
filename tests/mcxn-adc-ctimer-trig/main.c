/*
 * "CONVERT ON A TIMER MATCH" -- the OTHER canonical hardware-triggered ADC path,
 * the one a motor-control loop uses to pace current sampling with zero CPU work:
 *
 *     CTIMER0 match-3  ->  INPUTMUX (selector 5 -> ADC0_TRIG[0])
 *                      ->  ADC0 hardware trigger (TCTRL0[HTEN])
 *                      ->  ADC FIFO watermark  ->  eDMA request 21
 *                      ->  memory.
 *
 * THE CPU NEVER TRIGGERS A CONVERSION AND NEVER READS RESFIFO.  It arms the chain,
 * starts a periodic CTIMER match, and does nothing else.  Every result in `samples`
 * got there because a MATCH fired and a ROUTER carried the edge.
 *
 * THE GAP THIS CLOSES.  The LPTMR->ADC path already existed (tests/mcxn-adc-hwtrig),
 * but the CTIMER was wired to INPUTMUX for NOTHING: its match drove an NVIC IRQ and an
 * eDMA request, and its trigger-EVENT output -- the thing INPUTMUX routes to the ADC --
 * did not exist.  A guest doing kINPUTMUX_Ctimer0M3ToAdc0Trigger + a periodic match got
 * a timer that ticked and a trigger that went NOWHERE.  Same silent shape as before:
 * nothing logged, nothing faulted, not one conversion.
 *
 * Selector 5 = CTIMER0 match-3, DERIVED not guessed: NXP's compiled driver has
 * kINPUTMUX_Ctimer0M3ToAdc0Trigger = 5 + (ADC0_TRIG0 << 20), i.e. "write 5 into the
 * register at offset 0x280" = ADC0_TRIG[0].  M3 is the ADC-facing match.
 *
 * The sample value is OPERATOR-SET (the analog input is a QOM property -- a model must
 * not invent a voltage), so it is a golden the ADC cannot fabricate.
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

/* ---- clocks (mirror mcxn-ctimer: FIRC on, CTIMER0 <- FRO_HF 48 MHz) ------- */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define FIRCCSR_FIRCEN (1u << 0)
#define CTIMER0CLKSEL (*(volatile uint32_t *)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t *)0x400003D0u)
#define SEL_FROHF   3u

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
#define TCTRL_HTEN   (1u << 0)       /* hardware-trigger enable */

/* ---- CTIMER0 ------------------------------------------------------------- */
#define CT0 0x4000C000u
#define CT_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT_MCR (*(volatile uint32_t *)(CT0 + 0x14))
#define CT_MR3 (*(volatile uint32_t *)(CT0 + 0x18 + 3 * 4))
#define TCR_RUN 1u
#define TCR_RST 2u
#define MCR_RST3 (1u << 10)          /* reset TC on match 3 -> periodic match  */
#define MR3_COUNTS 300u

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

#define DMAREQ_ADC0_FIFO_A 21   /* CMSIS kDma0RequestMuxAdc0FifoARequest */
#define CHAN 3
#define NSAMP 4

static volatile uint32_t samples[NSAMP];

/* INPUTMUX0 (CMSIS): ADC0_TRIG[4] @0x280. */
#define INPUTMUX0    0x40006000u
#define ADC0_TRIG0   (*(volatile uint32_t *)(INPUTMUX0 + 0x280))
#define TRIG_SRC_CTIMER0_M3 5u       /* decoded from NXP's compiled driver */

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE;
    puts_("ADC-CTIMER-TRIG test\r\n");

    for (i = 0; i < NSAMP; i++) {
        samples[i] = 0xEEEEEEEEu;            /* poison: a stub leaves this */
    }

    /* CTIMER0 clocked from FRO_HF so its counter actually advances. */
    SCG_FIRCCSR   = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = SEL_FROHF;

    /* --- ADC: enabled, command 0 on channel 0, watermark 1 --------------- */
    ADC_CTRL   = CTRL_ADCEN;
    ADC_CMDL0  = 0;                          /* channel 0                   */
    ADC_CMDH0  = 0;
    ADC_TCTRL0 = (1u << 24);                 /* trigger 0 -> command 1      */
    ADC_FCTRL0 = (1u << 16);                 /* FWMARK = 1 (needs real FIFO)*/

    /* --- eDMA: ADC RESFIFO -> memory, two results per request ------------ */
    TCD_SADDR(CHAN)  = ADC_RESFIFO0_ADDR;    /* the FIFO does not advance   */
    TCD_SOFF(CHAN)   = 0;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 8;                    /* TWO results per request     */
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)samples;
    TCD_DOFF(CHAN)   = 4;
    TCD_DLAST(CHAN)  = (uint32_t)(-(int32_t)(4 * NSAMP));
    TCD_CITER(CHAN)  = NSAMP / 2;
    TCD_BITER(CHAN)  = NSAMP / 2;
    TCD_CSR(CHAN)    = TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_ADC0_FIFO_A;
    CH_CSR(CHAN)     = CSR_ERQ;               /* HARDWARE requests enabled   */
    ADC_DE           = DE_FWMDE0;             /* the ADC starts asking       */

    /* Route CTIMER0 match-3 -> ADC0_TRIG[0].  From here the CPU does nothing. */
    ADC0_TRIG0 = TRIG_SRC_CTIMER0_M3;

    /*
     * PHASE 1 -- ROUTED BUT NOT ARMED.  TCTRL0[HTEN] is still CLEAR.  A routed match
     * the guest has not enabled MUST NOT convert.  Run the periodic match anyway;
     * nothing may move.  Without this phase the test cannot tell a working HTEN gate
     * from a model that converts on ANY routed edge (more permissive than silicon).
     */
    CT_MR3 = MR3_COUNTS;
    CT_MCR = MCR_RST3;                       /* periodic: reset TC on match 3 */
    CT_TCR = TCR_RST;                        /* clear the counter             */
    CT_TCR = TCR_RUN;                        /* GO                            */
    for (d = 0; d < 400000; d++) {           /* let it match many times       */
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

    /* PHASE 2 -- ARM IT.  Now the periodic match really drives the conversions. */
    ADC_TCTRL0 |= TCTRL_HTEN;                /* arm the HARDWARE trigger    */

    /* Wait for the MATCH to have driven the whole major loop.  No CPU involvement. */
    for (d = 0; d < 20000000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }
    CT_TCR = 0;                              /* stop the timer              */

    /* THE CPU NEVER READ RESFIFO.  Everything here was carried by the DMA. */
    for (i = 0; i < NSAMP; i++) {
        puts_("  sample["); putc_((char)('0' + i)); puts_("] = ");
        puthex(samples[i]); puts_("\r\n");
        ok &= (samples[i] != 0xEEEEEEEEu);   /* it moved                    */
        ok &= !!(samples[i] & (1u << 31));   /* RESFIFO[VALID]              */
    }
    ok &= !!(CH_CSR(CHAN) & CSR_DONE);

    puts_(ok ? "CTIMER-TRIG PASS\r\n" : "CTIMER-TRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
