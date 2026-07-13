/*
 * INPUTMUX gates EVERY eDMA request: DMAn_REQ_ENABLE0..3, one bit per request
 * source.  RM 26.5.1.56: "0: DMA request to DMA0 and response from DMA0 are
 * blocked.  1: DMA request and response are enabled for DMA0."
 *
 * THE GAP THIS CLOSES.  That gate did not exist.  Our eDMA request lines were
 * UNGATED -- and that is NOT a harmless simplification.  It is the silent-wrong-
 * answer class INVERTED:
 *
 *     ⭐ THE MODEL WAS MORE PERMISSIVE THAN THE SILICON.  A model that is too
 *        FORGIVING does not fail safe -- IT SHIPS THE BUG TO THE HARDWARE.  A
 *        developer who forgets to enable the request passes here and dies on the
 *        board, and QEMU told him he was right.
 *
 * It reset to all-ones (all 122 lines enabled), which is exactly why nothing ever
 * broke while it was unmodelled -- every stock example still worked.  A gate that is
 * open by default is INVISIBLE UNTIL SOMEBODY CLOSES IT.  So this test closes it.
 *
 * WHY IT TESTS BOTH DIRECTIONS.  Proving the transfer works with the gate open
 * proves nothing about the gate -- that is just the old ungated behaviour.  The gate
 * only exists if CLOSING IT STOPS THE TRANSFER.  So:
 *
 *   PHASE 1: CLEAR the ADC0-FIFO-A bit (source 21) via DMA0_REQ_ENABLE0_CLR.  Run
 *            the exact same ADC+eDMA setup that mcxn-adc-dma proves works.  NOTHING
 *            MAY MOVE: the destination must still hold its poison and CH_CSR[DONE]
 *            must be clear.  An ungated model MOVES THE DATA HERE and fails.
 *   PHASE 2: SET the bit again via DMA0_REQ_ENABLE0_SET.  The pending request must
 *            now get through and the data must arrive -- so a model that simply
 *            broke the request line altogether cannot pass either.
 *
 * A gate that only ever blocks is a broken wire; a gate that only ever passes is
 * decoration.  It has to do BOTH, on command.
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

/* INPUTMUX0 DMA0_REQ_ENABLE0 and its SET/CLR write aliases (CMSIS offsets). */
#define INPUTMUX0            0x40006000u
#define DMA0_REQ_ENABLE0     (*(volatile uint32_t *)(INPUTMUX0 + 0x700))
#define DMA0_REQ_ENABLE0_SET (*(volatile uint32_t *)(INPUTMUX0 + 0x704))
#define DMA0_REQ_ENABLE0_CLR (*(volatile uint32_t *)(INPUTMUX0 + 0x708))
#define REQ_ADC0_FIFO_A      21              /* CMSIS dma_request_source_t */

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE;
    puts_("INPUTMUX-GATE test\r\n");

    /* Sanity: the gate is OPEN out of reset (RM: DMA0_REQ_ENABLE0 = FFFF_FFFF). */
    if (!(DMA0_REQ_ENABLE0 & (1u << REQ_ADC0_FIFO_A))) {
        puts_("  gate not open at reset -- wrong reset value\r\n");
        ok = 0;
    }

    /* PHASE 1: CLOSE the gate on ADC0 FIFO A.  Nothing may move. */
    DMA0_REQ_ENABLE0_CLR = (1u << REQ_ADC0_FIFO_A);
    if (DMA0_REQ_ENABLE0 & (1u << REQ_ADC0_FIFO_A)) {
        puts_("  CLR alias did not clear the bit\r\n");
        ok = 0;
    }

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
    for (i = 0; i < NSAMP; i++) {
        ADC_SWTRIG = 1;                      /* trigger 0                   */
    }
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

    /* Give the engine every chance to move something it must not move. */
    for (d = 0; d < 200000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }

    /* --- PHASE 1 VERDICT: the gate is CLOSED, so NOTHING may have moved. --- */
    for (i = 0; i < NSAMP; i++) {
        if (samples[i] != 0xEEEEEEEEu) {
            puts_("  BLOCKED-GATE LEAKED DATA: sample[");
            putc_((char)('0' + i));
            puts_("] = ");
            puthex(samples[i]);
            puts_("\r\n");
            ok = 0;
        }
    }
    if (CH_CSR(CHAN) & CSR_DONE) {
        puts_("  BLOCKED-GATE SET DONE\r\n");
        ok = 0;
    }
    puts_(ok ? "  phase1: gate CLOSED -> nothing moved (correct)\r\n"
             : "  phase1: FAILED\r\n");

    /* --- PHASE 2: RE-OPEN the gate.  The data must now arrive. ------------- */
    DMA0_REQ_ENABLE0_SET = (1u << REQ_ADC0_FIFO_A);
    for (i = 0; i < NSAMP; i++) {
        ADC_SWTRIG = 1;                      /* fresh conversions           */
    }
    for (d = 0; d < 2000000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }

    /* THE CPU NEVER READ RESFIFO.  Everything here was carried by the DMA. */
    for (i = 0; i < NSAMP; i++) {
        puts_("  sample["); putc_((char)('0' + i)); puts_("] = ");
        puthex(samples[i]); puts_("\r\n");
        ok &= (samples[i] != 0xEEEEEEEEu);   /* it moved                    */
        ok &= !!(samples[i] & (1u << 31));   /* RESFIFO[VALID]              */
    }
    ok &= !!(CH_CSR(CHAN) & CSR_DONE);   /* phase 2 must COMPLETE */

    puts_(ok ? "GATE PASS\r\n" : "GATE FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
