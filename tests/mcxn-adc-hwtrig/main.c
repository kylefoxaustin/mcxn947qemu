/*
 * "CONVERT ON A TIMER TICK" -- the canonical embedded ADC pattern, end to end:
 *
 *     LPTMR0 compare match  ->  INPUTMUX (selector 50 -> ADC0_TRIG[0])
 *                           ->  ADC0 hardware trigger (TCTRL0[HTEN])
 *                           ->  ADC FIFO watermark  ->  eDMA request 21
 *                           ->  memory.
 *
 * THE CPU NEVER TRIGGERS A CONVERSION AND NEVER READS RESFIFO.  It arms the chain
 * and then does nothing at all.  Every result in `samples` got there because a TIMER
 * fired and a ROUTER carried the edge.
 *
 * THE GAP THIS CLOSES.  There was NO hardware-trigger path.  The ONLY way to start a
 * conversion was a CPU write to SWTRIG -- so the single most common thing an ADC is
 * ever asked to do was IMPOSSIBLE, and the failure was SILENT.  The stock NXP
 * lpadc/edma example does exactly this (INPUTMUX_AttachSignal(INPUTMUX0, 0,
 * kINPUTMUX_Lptmr0ToAdc0Trigger); LPTMR_StartTimer(...)), and on this model the timer
 * ticked, THE TRIGGER WENT NOWHERE, and NOT ONE CONVERSION EVER HAPPENED.  Nothing
 * logged.  Nothing faulted.  It just sat there.
 *
 *     ⭐ A ROUTER THAT ROUTES NOTHING LOOKS EXACTLY LIKE A ROUTER.  INPUTMUX stored
 *        every selector faithfully and read it back faithfully, and the model's own
 *        comment called that "faithful" -- because nothing was connected to the far
 *        end.  The stub asserted its own irrelevance and the assertion was
 *        self-fulfilling.
 *
 * Selector 50 = LPTMR0 is DERIVED, NOT GUESSED: NXP's compiled driver has
 * kINPUTMUX_Lptmr0ToAdc0Trigger = 0x2800_0032, and INPUTMUX_AttachSignal() does
 *     *(base + (conn >> 20) + idx * 4) = conn & 0xFFFFF
 * i.e. "write 50 into the register at offset 0x280" = ADC0_TRIG[0].
 *
 * The sample value is OPERATOR-SET (the analog input is a QOM property -- a model
 * must not invent a voltage), so it is a golden the ADC cannot fabricate.
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

/* ---- ADC0 ---------------------------------------------------------------- */
#define ADC0 0x4010D000u
#define ADC_CTRL   (*(volatile uint32_t *)(ADC0 + 0x010))
#define ADC_STAT   (*(volatile uint32_t *)(ADC0 + 0x014))
#define ADC_DE     (*(volatile uint32_t *)(ADC0 + 0x01C))
#define ADC_SWTRIG (*(volatile uint32_t *)(ADC0 + 0x034))   /* WO */
#define ADC_FCTRL0 (*(volatile uint32_t *)(ADC0 + 0x0E0))
#define ADC_RESFIFO0_ADDR         (ADC0 + 0x300)
#define ADC_TCTRL0 (*(volatile uint32_t *)(ADC0 + 0x0A0))
#define ADC_CMDL0  (*(volatile uint32_t *)(ADC0 + 0x100))
#define ADC_CMDH0  (*(volatile uint32_t *)(ADC0 + 0x104))

#define CTRL_ADCEN   (1u << 0)
#define DE_FWMDE0    (1u << 0)

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
#define TRIG_SRC_LPTMR0 50u          /* decoded from NXP's compiled driver */

/* LPTMR0 (CMSIS base 0x4004A000). */
#define LPTMR0       0x4004A000u
#define LPTMR_CSR    (*(volatile uint32_t *)(LPTMR0 + 0x0))
#define LPTMR_PSR    (*(volatile uint32_t *)(LPTMR0 + 0x4))
#define LPTMR_CMR    (*(volatile uint32_t *)(LPTMR0 + 0x8))
#define LPTMR_CSR_TEN (1u << 0)
#define LPTMR_CSR_TFC (1u << 2)
#define LPTMR_PSR_PBYP (1u << 2)     /* bypass the prescaler */

#define TCTRL_HTEN   (1u << 0)       /* hardware-trigger enable */

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE;
    puts_("ADC-HWTRIG test\r\n");

    for (i = 0; i < NSAMP; i++) {
        samples[i] = 0xEEEEEEEEu;            /* poison: a stub leaves this */
    }

    /* --- ADC: enabled, command 0 on channel 0, watermark 0 --------------- */
    ADC_CTRL   = CTRL_ADCEN;
    ADC_CMDL0  = 0;                          /* channel 0                   */
    ADC_CMDH0  = 0;
    ADC_TCTRL0 = (1u << 24);                 /* trigger 0 -> command 1      */
    /*
     * FWMARK = 1: the ADC asks for service only when it holds MORE THAN ONE
     * result, and each request drains two (NBYTES = 8).  This is how the stock
     * LPADC EDMA driver actually runs -- and it is why this test now REQUIRES A
     * REAL FIFO.
     *
     * ⭐ WITH FWMARK = 0 THIS TEST COULD NOT SEE THE BUG IT WAS WRITTEN FOR.
     * I regressed the FIFO to depth 1 on purpose and the test STILL PASSED: at
     * FWMARK = 0 the eDMA drains the single slot between triggers and keeps up, so
     * "depth 1" and "depth 16" look identical from here.  The mutation is what told
     * me; the test's own comment had confidently claimed the opposite.
     *
     * At FWMARK = 1 a depth-1 FIFO can never exceed the watermark -- occupancy is
     * 0 or 1, never 2 -- so NO REQUEST CAN EVER FIRE and nothing moves.  The
     * watermark is the feature the depth EXISTS FOR, so testing the watermark is
     * what tests the depth.
     */
    ADC_FCTRL0 = (1u << 16);                 /* FWMARK = 1                  */

    /* --- eDMA: ADC RESFIFO -> memory, one result per request ------------- */
    TCD_SADDR(CHAN)  = ADC_RESFIFO0_ADDR;    /* the FIFO does not advance   */
    TCD_SOFF(CHAN)   = 0;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 8;                    /* TWO results per request     */
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)samples;
    TCD_DOFF(CHAN)   = 4;                    /* walk the destination        */
    TCD_DLAST(CHAN)  = (uint32_t)(-(int32_t)(4 * NSAMP));
    /* NBYTES moves TWO samples, so the major loop is NSAMP/2 minor loops. */
    TCD_CITER(CHAN)  = NSAMP / 2;
    TCD_BITER(CHAN)  = NSAMP / 2;
    TCD_CSR(CHAN)    = TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_ADC0_FIFO_A;   /* listen to ADC0 FIFO A       */

    CH_CSR(CHAN) = CSR_ERQ;                  /* HARDWARE requests enabled   */
    ADC_DE = DE_FWMDE0;                      /* the ADC starts asking       */

    /* --- convert: each software trigger fills the FIFO, which asks the DMA */
    /*
     * Route LPTMR0 -> ADC0_TRIG[0] and ARM the hardware trigger.  From here the CPU
     * does NOTHING: no SWTRIG, no RESFIFO read.  If the trigger path is broken the
     * destination keeps its poison and this fails -- which is exactly what happened
     * before INPUTMUX routed anything.
     */
    ADC0_TRIG0 = TRIG_SRC_LPTMR0;

    /*
     * PHASE 1 -- ROUTED BUT NOT ARMED.  TCTRL0[HTEN] is still CLEAR.  The RM calls
     * that bit "Trigger Enable", and a trigger the guest has not enabled MUST NOT
     * CONVERT.  Run the timer anyway; nothing may move.
     *
     * Without this phase the test could not tell a working HTEN gate from a model
     * that converts on ANY routed edge -- and a model that converts when it was not
     * asked to is MORE PERMISSIVE THAN THE SILICON, which is how a developer's bug
     * gets a green light here and dies on the board.
     */
    LPTMR_PSR = LPTMR_PSR_PBYP;              /* count the raw clock         */
    LPTMR_CMR = 200;                         /* a short compare period      */
    LPTMR_CSR = LPTMR_CSR_TEN;               /* GO                          */
    for (d = 0; d < 400000; d++) {           /* let it tick many times      */
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

    /* PHASE 2 -- ARM IT.  Now the timer really does drive the conversions. */
    ADC_TCTRL0 |= TCTRL_HTEN;                /* arm the HARDWARE trigger    */
    /*
     * ⭐ THERE IS DELIBERATELY NO DELAY BETWEEN THE TRIGGERS.
     *
     * There used to be:  for (d = 0; d < 2000; d++) { }  -- "let the bottom half
     * run".  I wrote that crutch to make this test pass, and it was papering over
     * a REAL BUG: the ADC result FIFO was a SINGLE SLOT, so a second conversion
     * arriving before the eDMA drained the first SILENTLY OVERWROTE it.  The delay
     * gave the DMA time to drain between triggers, so the model looked fine.
     *
     * Without the delay all four conversions land back-to-back and MUST QUEUE --
     * which is what a 16-deep FIFO is for.  If the FIFO ever regresses to depth 1,
     * samples go missing here and this fails.  The crutch WAS the camouflage.
     */

    /* Wait for the TIMER to have driven the whole major loop.  No CPU involvement. */
    for (d = 0; d < 20000000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }
    LPTMR_CSR = 0;                           /* stop the timer              */

    /* THE CPU NEVER READ RESFIFO.  Everything here was carried by the DMA. */
    for (i = 0; i < NSAMP; i++) {
        puts_("  sample["); putc_((char)('0' + i)); puts_("] = ");
        puthex(samples[i]); puts_("\r\n");
        ok &= (samples[i] != 0xEEEEEEEEu);   /* it moved                    */
        ok &= !!(samples[i] & (1u << 31));   /* RESFIFO[VALID]              */
    }
    ok &= !!(CH_CSR(CHAN) & CSR_DONE);

    puts_(ok ? "HWTRIG PASS\r\n" : "HWTRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
