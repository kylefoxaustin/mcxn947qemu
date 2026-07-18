/*
 * MCXN947 CTIMER match -> eDMA (match-paced transfer, stock-driver-shaped).
 *
 * A CTIMER match event drives an eDMA request line (CMSIS CTIMER0 M0 = source 7).  The
 * CTIMER has NO DMA-enable bit of its own -- the match event itself raises the request,
 * and INPUTMUX gates it (modelled in the eDMA).  It is a one-shot PULSE: one match moves
 * exactly one minor loop.  Before the match request line was wired, a match-paced DMA
 * (the classic "timer meters data out at a fixed rate" pattern) moved nothing: the channel
 * set ERQ and waited for a request that could never assert.
 *
 * Oracle the CTIMER/eDMA models do not own, on two axes:
 *   DATA -- a const pattern in flash is copied word-by-word into an SRAM buffer entirely by
 *           match-paced DMA; the CPU never writes the buffer.  A dead request line leaves it
 *           zero (and never sets DONE).
 *   RATE -- one word per match.  We first CALIBRATE one match period against SysTick (an
 *           independent Arm core timer, no dependence on the modelled CTIMER clock), then
 *           demand the whole 8-word transfer take at least 7 periods.  A model that drained
 *           the buffer in a single match would finish in ~1 period -- caught by the floor.
 *
 * ⚠ -icount shift=3 REQUIRED: the RATE axis measures elapsed VIRTUAL time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LP + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LP + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LP + 0x1C))
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

/* ---- clocks (mirror the mcxn-ctimer test: FIRC on, CTIMER0 <- FRO_HF 48 MHz) ---- */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_FIRCCFG (*(volatile uint32_t *)(SCG0 + 0x308))
#define FIRCCSR_FIRCEN (1u << 0)
#define CTIMER0CLKSEL (*(volatile uint32_t *)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t *)0x400003D0u)
#define SEL_FROHF   3u

/* ---- CTIMER0 ------------------------------------------------------------- */
#define CT0 0x4000C000u
#define CT_IR  (*(volatile uint32_t *)(CT0 + 0x00))
#define CT_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT_PR  (*(volatile uint32_t *)(CT0 + 0x0C))
#define CT_MCR (*(volatile uint32_t *)(CT0 + 0x14))
#define CT_MR0 (*(volatile uint32_t *)(CT0 + 0x18))
#define TCR_RUN 1u
#define TCR_RST 2u
#define MCR_INT0 (1u << 0)
#define MCR_RST0 (1u << 1)
#define MR0_COUNTS 200u

/* ---- SysTick ------------------------------------------------------------- */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

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
#define ATTR_32BIT   0x0202u
#define CSR_ERQ      (1u << 0)
#define CSR_DONE     (1u << 30)
#define TCD_INTMAJOR (1u << 1)
#define TCD_DREQ     (1u << 3)
#define DMAREQ_CTIMER0_M0 7u
#define CHAN 0
#define NW   8

static const uint32_t pat[NW] = {
    0xC0DED00Du, 0x0BADF00Du, 0xFEEDFACEu, 0x8BADF00Du,
    0x1BADB002u, 0xDEADC0DEu, 0xCAFEBABEu, 0xB16B00B5u,
};
static volatile uint32_t out[NW];

/* Time one MR0 match period against SysTick, then hold the counter in reset. */
static uint32_t one_period(void)
{
    uint32_t t0, t1;

    CT_TCR = TCR_RST;
    CT_IR  = 0xFF;
    t0 = SYST_CVR;
    CT_TCR = TCR_RUN;
    while (!(CT_IR & 1u)) {
    }
    t1 = SYST_CVR;
    CT_TCR = TCR_RST;
    CT_IR  = 0xFF;
    return (t0 - t1) & SYST_MASK;        /* SysTick counts DOWN */
}

void cpu0_main(void)
{
    uint32_t period, total, t0, t1;
    int ok = 1, i, g;

    LP_CTRL = (1u << 19);
    puts_("CTIMER-DMA test\r\n");

    /* FRO_HF on, CTIMER0 <- FRO_HF /1; SysTick free-running on the core clock. */
    SCG_FIRCCFG = 0;
    SCG_FIRCCSR = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = SEL_FROHF;
    SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = (1u << 0) | (1u << 2);

    /* Periodic match-0: reset the counter each match (so it repeats) and flag IR[0]. */
    CT_PR  = 0;
    CT_MR0 = MR0_COUNTS;
    CT_MCR = MCR_INT0 | MCR_RST0;

    /* CALIBRATE one period (a stray pulse here hits no armed channel and evaporates). */
    period = one_period();

    /* Arm ch0: pat[] -> out[], one 32-bit word per match request, NW words. */
    CH_CSR(CHAN)     = CSR_DONE;         /* W1C stale DONE / INT from a prior run  */
    CH_INT(CHAN)     = 1u;
    TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)pat;
    TCD_SOFF(CHAN)   = 4;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 4;
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)out;
    TCD_DOFF(CHAN)   = 4;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = NW;
    TCD_BITER(CHAN)  = NW;
    TCD_CSR(CHAN)    = TCD_INTMAJOR | TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_CTIMER0_M0;
    CH_CSR(CHAN)     = CSR_ERQ;

    /* Run the timer; each match paces one word.  Bounded so a dead line FAILs, not hangs. */
    t0 = SYST_CVR;
    CT_TCR = TCR_RUN;
    g = 20000000;
    while (!(CH_CSR(CHAN) & CSR_DONE) && g--) {
    }
    t1 = SYST_CVR;
    CT_TCR = TCR_RST;
    total = (t0 - t1) & SYST_MASK;

    ok &= (CH_CSR(CHAN) & CSR_DONE) && !(CH_CSR(CHAN) & CSR_ERQ) && (CH_INT(CHAN) & 1u);
    for (i = 0; i < NW; i++) {
        ok &= (out[i] == pat[i]);        /* DATA: match-paced DMA carried it        */
    }
    ok &= (total >= (uint32_t)(NW - 1) * period);   /* RATE: one word per match      */

    puts_(ok ? "CTIMER-DMA PASS\r\n" : "CTIMER-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[60] = { [0] = (vec_t)0x20010000u, [1] = cpu0_main };
