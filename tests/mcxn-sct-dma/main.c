/*
 * MCXN947 SCT event -> eDMA (event-paced transfer, stock-driver-shaped).
 *
 * An SCT event drives an eDMA request line (CMSIS SCT0 DMA0 = source 19).  The SCT's
 * DMAREQ0 register selects which events feed request 0 (DEV_0 = event 0, the modelled
 * match/limit event); the event itself is the trigger, as a one-shot PULSE -- one event,
 * one minor loop.  Before the SCT request line was wired, an event-paced DMA moved
 * nothing: the channel set ERQ and waited for a request that could never assert.
 *
 * Oracle the SCT/eDMA models do not own, on two axes:
 *   DATA -- a const pattern in flash is copied into an SRAM buffer entirely by event-paced
 *           DMA; the CPU never writes the buffer.  A dead request line leaves it zero.
 *   RATE -- one word per event.  We CALIBRATE one SCT period against SysTick (independent
 *           of the modelled SCT clock), then demand the 8-word transfer take at least 7
 *           periods.  A model draining the buffer in one event finishes in ~1 period.
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

/* ---- clocks (mirror mcxn-sct: FIRC on, SCT <- FRO_HF 48 MHz) ------------- */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_FIRCCFG (*(volatile uint32_t *)(SCG0 + 0x308))
#define FIRCCSR_FIRCEN (1u << 0)
#define SCTCLKSEL (*(volatile uint32_t *)0x400002F0u)
#define SCTCLKDIV (*(volatile uint32_t *)0x400003B4u)
#define SEL_FROHF 3u

/* ---- SCT0 ---------------------------------------------------------------- */
#define SCT0 0x40091000u
#define SCT_CTRL      (*(volatile uint32_t *)(SCT0 + 0x004))
#define SCT_DMAREQ0   (*(volatile uint32_t *)(SCT0 + 0x05C))
#define SCT_EVEN      (*(volatile uint32_t *)(SCT0 + 0x0F0))
#define SCT_EVFLAG    (*(volatile uint32_t *)(SCT0 + 0x0F4))
#define SCT_MATCHREL0 (*(volatile uint32_t *)(SCT0 + 0x180))
#define CTRL_HALT_L   (1u << 2)
#define CTRL_CLRCTR_L (1u << 3)
#define EV0           (1u << 0)
#define DMAREQ_DEV0   (1u << 0)
#define MATCHREL      0x200u

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
#define DMAREQ_SCT0_DMA0 19u
#define CHAN 0
#define NW   8

static const uint32_t pat[NW] = {
    0xC0DED00Du, 0x0BADF00Du, 0xFEEDFACEu, 0x8BADF00Du,
    0x1BADB002u, 0xDEADC0DEu, 0xCAFEBABEu, 0xB16B00B5u,
};
static volatile uint32_t out[NW];

/* Time one SCT event period against SysTick, then halt the counter. */
static uint32_t one_period(void)
{
    uint32_t t0, t1;

    SCT_CTRL = CTRL_HALT_L | CTRL_CLRCTR_L;
    SCT_EVFLAG = EV0;
    t0 = SYST_CVR;
    SCT_CTRL = 0;                         /* clear HALT -> run */
    while (!(SCT_EVFLAG & EV0)) {
    }
    t1 = SYST_CVR;
    SCT_CTRL = CTRL_HALT_L;
    SCT_EVFLAG = EV0;
    return (t0 - t1) & SYST_MASK;
}

void cpu0_main(void)
{
    uint32_t period, total, t0, t1;
    int ok = 1, i, g;

    LP_CTRL = (1u << 19);
    puts_("SCT-DMA test\r\n");

    SCG_FIRCCFG = 0;
    SCG_FIRCCSR = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    SCTCLKDIV = 0;
    SCTCLKSEL = SEL_FROHF;
    SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = (1u << 0) | (1u << 2);

    SCT_EVEN = 0;                        /* no NVIC interrupt: we poll EVFLAG   */
    SCT_MATCHREL0 = MATCHREL;

    /* CALIBRATE one period (a stray event pulse here hits no armed channel, evaporates). */
    period = one_period();

    /* Route event 0 -> DMA request 0, then arm ch0: pat[] -> out[], one word per event. */
    SCT_DMAREQ0 = DMAREQ_DEV0;
    CH_CSR(CHAN)     = CSR_DONE;
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
    CH_MUX(CHAN)     = DMAREQ_SCT0_DMA0;
    CH_CSR(CHAN)     = CSR_ERQ;

    /* Run the counter; each event paces one word.  Bounded so a dead line FAILs. */
    SCT_CTRL = CTRL_CLRCTR_L;           /* clear the counter and run (HALT cleared) */
    t0 = SYST_CVR;
    g = 20000000;
    while (!(CH_CSR(CHAN) & CSR_DONE) && g--) {
    }
    t1 = SYST_CVR;
    SCT_CTRL = CTRL_HALT_L;
    total = (t0 - t1) & SYST_MASK;

    ok &= (CH_CSR(CHAN) & CSR_DONE) && !(CH_CSR(CHAN) & CSR_ERQ) && (CH_INT(CHAN) & 1u);
    for (i = 0; i < NW; i++) {
        ok &= (out[i] == pat[i]);        /* DATA: event-paced DMA carried it        */
    }
    ok &= (total >= (uint32_t)(NW - 1) * period);   /* RATE: one word per event      */

    puts_(ok ? "SCT-DMA PASS\r\n" : "SCT-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
