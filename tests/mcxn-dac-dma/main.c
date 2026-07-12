/*
 * MCXN947 PERIPHERAL-TRIGGERED eDMA — DAC waveform streamed by the DMA.
 *
 * Companion to tests/mcxn-sai-dma, and the check that the eDMA's request-MUX
 * routing is not SAI-specific: this exercises a DIFFERENT request source (DAC0
 * = mux source 25, vs SAI0 Tx = 100) through the same machinery.
 *
 * Shaped like a stock DAC driver streaming a waveform: the CPU fills a sample
 * buffer in memory, arms an eDMA channel pointed at the DAC's DATA register,
 * enables the DAC's watermark DMA request (DER[WM_DMAEN]) — and then NEVER
 * WRITES DATA AGAIN.  Every sample that reaches the converter is carried there
 * by the DMA, one minor loop per request, because the FIFO asked for it.
 *
 *   THE CPU NEVER WRITES DAC_DATA.  If the request line does not work, the FIFO
 *   stays empty, every trigger UNDERFLOWS, and the test fails.  It cannot pass
 *   by accident.
 *
 * The guest cannot read back what a DAC converted — a DAC's answer only exists
 * on the pin.  So the OPERATOR probes it: the analog output is exposed as a
 * read-only QOM property and the harness reads it over QMP after each trigger,
 * which is exactly what a bench engineer does with a scope.  The golden is the
 * sample buffer chosen here; the observation point is outside the register file.
 *
 * The guest prints a marker per converted sample; run.sh reads the pin at each
 * marker and asserts the sequence is BYTE-EXACT and IN ORDER.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE_U (1u << 19)
#define CTRL_RE_U (1u << 18)
#define STAT_TDRE (1u << 23)
#define STAT_RDRF (1u << 21)

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

/*
 * Wait for the operator.  The pin holds only the MOST RECENT sample, so if the
 * guest converts the whole waveform before the harness gets round to probing,
 * every read returns the last value and the ORDER is unverifiable.  The guest
 * must therefore stop after each conversion until the scope has been read.
 * (This bit me: the first version raced ahead and the operator saw 0x1 six
 * times -- the last sample, six times over.  A test that samples an
 * overwrite-in-place output MUST synchronise with it.)
 */
static void wait_for_probe(void)
{
    while (!(LP_STAT & STAT_RDRF)) {
    }
    (void)LP_DATA;
}

/* ---- DAC0 ---------------------------------------------------------------- */
#define DAC0 0x4010F000u
#define DAC_DATA_ADDR (DAC0 + 0x08)
#define DAC_GCR (*(volatile uint32_t *)(DAC0 + 0x0C))
#define DAC_FCR (*(volatile uint32_t *)(DAC0 + 0x10))
#define DAC_FPR (*(volatile uint32_t *)(DAC0 + 0x14))   /* RO */
#define DAC_TCR (*(volatile uint32_t *)(DAC0 + 0x28))   /* WO */
#define DAC_FSR (*(volatile uint32_t *)(DAC0 + 0x18))
#define DAC_DER (*(volatile uint32_t *)(DAC0 + 0x20))

#define GCR_DACEN  (1u << 0)
#define GCR_FIFOEN (1u << 3)
#define TCR_SWTRG  (1u << 0)
#define FSR_UF     (1u << 7)    /* underflow: the FIFO had nothing to convert */
#define DER_WM_DMAEN (1u << 2)  /* LPDAC_DER_WM_DMAEN_MASK */

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

#define CSR_ERQ      (1u << 0)
#define CSR_DONE     (1u << 30)
#define TCD_DREQ     (1u << 3)
#define ATTR_32BIT   ((2u << 8) | 2u)

#define DMAREQ_DAC0 25          /* CMSIS kDma0RequestMuxDac0FifoRequest */

#define NSAMPLES 6
#define CHAN 1

/* Zero-init and filled at run time: there is no startup .data copy. */
static volatile uint32_t samples[NSAMPLES];

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE_U | CTRL_RE_U;
    puts_("DAC-DMA test\r\n");

    /* A 12-bit waveform nothing in the model could invent. */
    samples[0] = 0x0ABC; samples[1] = 0x0123; samples[2] = 0x0FFF;
    samples[3] = 0x0555; samples[4] = 0x0AAA; samples[5] = 0x0001;

    /* --- DAC: FIFO mode, watermark 3, DMA request on watermark ------------ */
    DAC_FCR = 3;                             /* WML = 3                      */
    DAC_GCR = GCR_DACEN | GCR_FIFOEN;
    DAC_FSR = FSR_UF;                        /* W1C any stale underflow      */

    /* --- eDMA: memory -> DAC DATA, one sample per request ----------------- */
    TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)samples;
    TCD_SOFF(CHAN)   = 4;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 4;                    /* ONE sample per DMA request   */
    TCD_SLAST(CHAN)  = (uint32_t)(-(int32_t)(4 * NSAMPLES));
    TCD_DADDR(CHAN)  = DAC_DATA_ADDR;        /* the FIFO port does not move  */
    TCD_DOFF(CHAN)   = 0;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = NSAMPLES;
    TCD_BITER(CHAN)  = NSAMPLES;
    TCD_CSR(CHAN)    = TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_DAC0;          /* listen to the DAC's FIFO line */

    CH_CSR(CHAN) = CSR_ERQ;                  /* HARDWARE requests enabled     */

    /*
     * Arming the watermark DMA request is the moment the DAC starts asking: its
     * FIFO is empty (occupancy <= WML), so it asserts and the eDMA fills it —
     * WITHOUT THE CPU EVER WRITING DAC_DATA.
     */
    DAC_DER = DER_WM_DMAEN;

    /* Let the (asynchronous) bottom half run. */
    for (d = 0; d < 1000; d++) {
    }

    /*
     * Convert the waveform.  Each trigger pops one sample to the pin; the FIFO
     * drops back to the watermark and asks the DMA for the next one.  The
     * operator reads the pin at every marker.
     */
    for (i = 0; i < NSAMPLES; i++) {
        DAC_TCR = TCR_SWTRG;
        for (d = 0; d < 1000; d++) {         /* let the refill request settle */
        }
        puts_("TRIG\r\n");                   /* harness probes the pin here   */
        wait_for_probe();                    /* ...and must finish before we
                                              * overwrite the pin              */
    }

    /* Nothing underflowed: the DMA kept the converter fed the whole time. */
    ok &= !(DAC_FSR & FSR_UF);
    /* And the major loop really ran to completion. */
    ok &= !!(CH_CSR(CHAN) & CSR_DONE);
    ok &= !(CH_CSR(CHAN) & CSR_ERQ);         /* DREQ auto-cleared it          */

    puts_(ok ? "DACDMA GUEST-OK\r\n" : "DACDMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
