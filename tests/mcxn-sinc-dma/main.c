/*
 * MCXN947 SINC -> eDMA data path (peripheral-triggered, stock-driver-shaped).
 *
 * The SINC computes real CIC results into a per-channel FIFO.  A DMA-driven SINC
 * driver (the stock SINC_TransferReceiveEDMA shape) arms an eDMA channel at CnRDATA,
 * sets CnCCR[DMAEN], and NEVER READS CnRDATA ITSELF -- every settled result must be
 * carried out by the DMA when the channel's FIFO passes its watermark.
 *
 * Before the per-channel request line existed, CnCCR[DMAEN] drove nothing: the SINC
 * never raised an eDMA request, the channel (ERQ set) waited forever, and the results
 * the filter genuinely computed never moved.  This test fails if that is still true.
 *
 * The oracle is closed-form and independent of the DMA path: an all-ones bitstream into
 * a CIC of order ORD, OSR settles to the DC gain OSR^ORD.  At ORD=1, OSR=16 that is 16,
 * and CnRDATA returns it in bits [31:8] -- so every DMA'd word must read back (w>>8)==16.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP_CTRL (*(volatile uint32_t *)0x400B4018u)
#define LP_STAT (*(volatile uint32_t *)0x400B4014u)
#define LP_DATA (*(volatile uint32_t *)0x400B401Cu)
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

/* ---- SINC (channel 0) ---------------------------------------------------- */
#define SINC       0x40108000u
#define SINC_MCR   (*(volatile uint32_t *)(SINC + 0x08))
#define SINC_NIS   (*(volatile uint32_t *)(SINC + 0x18))
#define SINC_SR    (*(volatile uint32_t *)(SINC + 0x24))
#define C0_CCR     (*(volatile uint32_t *)(SINC + 0x38))
#define C0_CDR     (*(volatile uint32_t *)(SINC + 0x3C))
#define C0_CCFR    (*(volatile uint32_t *)(SINC + 0x40))
#define C0_CBIAS   (*(volatile uint32_t *)(SINC + 0x48))
#define C0_CRDATA_ADDR 0x40108054u
#define C0_CRDATA  (*(volatile uint32_t *)C0_CRDATA_ADDR)
#define C0_CMPDATA (*(volatile uint32_t *)(SINC + 0x58))
#define C0_CSR     (*(volatile uint32_t *)(SINC + 0x60))

#define MCR_STRIG0    (1u << 0)
#define MCR_MEN       (1u << 15)
#define CCR_CHEN      (1u << 0)
#define CCR_PFEN      (1u << 1)
#define CCR_DMAEN     (1u << 3)
#define CCR_FIFOEN    (1u << 14)
#define CCFR_RDFMT    (1u << 6)     /* unsigned result */
#define CCFR_IBFMT_PM (2u << 16)    /* register-fed parallel mode */
#define CSR_PSRDY     (1u << 7)
#define SR_FIFOEMPTY0 (1u << 16)

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
#define ATTR_32BIT   0x0202u        /* SSIZE=DSIZE=2 (4 bytes) */
#define CSR_ERQ      (1u << 0)
#define CSR_DONE     (1u << 30)
#define TCD_INTMAJOR (1u << 1)
#define TCD_DREQ     (1u << 3)

#define DMAREQ_SINC0_CH0 103u
#define CHAN 0
#define NRES 4                       /* settled results carried by the DMA */
#define DCGAIN 16                    /* OSR^ORD at ORD=1, OSR=16 */

static volatile uint32_t out[NRES];

static void feed_window(void)        /* one 16-bit all-ones word = one OSR=16 window */
{
    while (!(C0_CSR & CSR_PSRDY)) {
    }
    C0_CMPDATA = 0xFFFFu;
}

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    uint32_t i;

    LP_CTRL = (1u << 19);
    puts_("SINC-DMA test\r\n");

    /* --- SINC ch0: ORD=1, OSR=16, continuous PM mode, watermark 0 --------- */
    C0_CCR   = 0;                                 /* reset filter state */
    SINC_NIS = 0xFFFFFFFFu;
    C0_CDR   = (16u - 1u) | (1u << 11) | (1u << 14);   /* PFOSR=15, PFORD=1, continuous */
    C0_CCFR  = CCFR_RDFMT | CCFR_IBFMT_PM | (0u << 10); /* watermark = 0 */
    C0_CBIAS = 0;
    C0_CCR   = CCR_CHEN | CCR_PFEN | CCR_FIFOEN;   /* NB: DMAEN not yet set */
    SINC_MCR = MCR_MEN | MCR_STRIG0;

    /* Warm up so the comb has settled -- drain via CPU so the FIFO starts empty. */
    for (i = 0; i < 6; i++) {
        feed_window();
        while (!(SINC_SR & SR_FIFOEMPTY0)) {
            (void)C0_CRDATA;
        }
    }

    /* --- eDMA channel: SINC CnRDATA (fixed) -> out[] (advancing) ---------- */
    TCD_SADDR(CHAN)  = C0_CRDATA_ADDR;             /* the FIFO register, does not advance */
    TCD_SOFF(CHAN)   = 0;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 4;                          /* ONE result per DMA request */
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)out;
    TCD_DOFF(CHAN)   = 4;                          /* walk the destination buffer */
    TCD_DLAST(CHAN)  = (uint32_t)(-(int32_t)(4 * NRES));
    TCD_CITER(CHAN)  = NRES;
    TCD_BITER(CHAN)  = NRES;
    TCD_CSR(CHAN)    = TCD_INTMAJOR | TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_SINC0_CH0;           /* listen to SINC0 ch0's request line */
    CH_CSR(CHAN)     = CSR_ERQ;                    /* HARDWARE requests enabled */

    /*
     * ⭐ NOTHING HAS MOVED YET: the FIFO is empty (warm-up drained it), so the SINC has
     *   raised no request, and CnCCR[DMAEN] is not even set.  This is the negative anchor.
     */
    ok &= (out[0] == 0);

    /* --- arm the request line: set DMAEN, then feed settled results ------- */
    C0_CCR = CCR_CHEN | CCR_PFEN | CCR_FIFOEN | CCR_DMAEN;

    for (i = 0; i < NRES; i++) {
        feed_window();                 /* each window -> one settled result -> one request */
    }

    /* The DMA runs ASYNCHRONOUSLY (bottom half).  Wait for the major loop. */
    for (d = 0; d < 20000000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }

    /* --- the results were carried by the DMA, byte-exact and settled ------ */
    ok &= !!(CH_CSR(CHAN) & CSR_DONE);             /* major loop complete */
    ok &= !(CH_CSR(CHAN) & CSR_ERQ);               /* DREQ auto-cleared the request */
    ok &= !!(CH_INT(CHAN) & 1u);                   /* INTMAJOR latched */
    for (i = 0; i < NRES; i++) {
        ok &= ((out[i] >> 8) == DCGAIN);           /* the CIC DC gain, carried by DMA */
    }

    puts_(ok ? "SINC-DMA PASS\r\n" : "SINC-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
