/*
 * MCXN947 PERIPHERAL-TRIGGERED eDMA — SAI audio moved entirely by the DMA.
 *
 * THE GAP THIS CLOSES.  The eDMA modelled only the SOFTWARE trigger: a write to
 * TCD_CSR[START] ran the whole major loop in one go.  CH_CSR[ERQ] — the bit that
 * enables HARDWARE requests — was DEFINED AND NEVER READ, a dead constant, and
 * no peripheral had a request line at all.  So peripheral-triggered DMA, which
 * is how essentially every real audio / ADC / UART / SPI transfer works, DID NOT
 * EXIST: a guest that armed a channel, set ERQ, and waited for the SAI's FIFO
 * watermark to drive the transfer waited FOREVER.  Nothing could raise a
 * request, so nothing ever moved.  (Found via 95emulator's sharper framing of
 * my own audit: a source peripheral legitimately owns zero DMA calls — its
 * MOVER is the eDMA — so the question is not "does this model call DMA" but
 * "is the request line actually wired?"  Mine was not.)
 *
 * This test is shaped like a STOCK DRIVER (SAI_TransferSendEDMA / Zephyr's
 * i2s_mcux_sai): the CPU writes the sample buffer into memory, arms an eDMA
 * channel pointed at the SAI's TDR, enables the SAI's FIFO-request DMA line
 * (TCSR[FRDE]) — and then NEVER TOUCHES TDR AGAIN.  Every word that reaches the
 * transmitter is carried there by the DMA, one minor loop per request, exactly
 * as the hardware does it.
 *
 *   THE CPU NEVER WRITES TDR.  If the request lines do not work, ZERO words are
 *   transmitted and the test fails — it cannot pass by accident.
 *
 * With SAI_TXD jumpered to SAI_RXD (the board-level loopback, not an invented
 * register bit), the words come back on the receive side and must match the
 * source buffer BYTE-EXACT and IN ORDER.
 *
 * Also pinned:
 *   - the major-loop interrupt fires once the last minor loop lands (INTMAJOR);
 *   - TCD_CSR[DREQ] auto-clears ERQ at major completion, so the channel stops
 *     asking.  A model that kept servicing would over-run the buffer.
 *
 * Prints "SAIDMA PASS" only if every check holds.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE_U (1u << 19)
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


/* ---- SAI0 ---------------------------------------------------------------- */
#define SAI0 0x40106000u
#define SAI_TCSR (*(volatile uint32_t *)(SAI0 + 0x08))
#define SAI_TCR1 (*(volatile uint32_t *)(SAI0 + 0x0C))
#define SAI_TCR2 (*(volatile uint32_t *)(SAI0 + 0x10))
/*
 * ⭐ TCR2[BCD] -- BIT CLOCK DIRECTION.  RM: "0b - Generate externally in Target mode;
 *   1b - Generate internally in Controller mode."
 *
 * THIS TEST USED TO WRITE TCR2 WITHOUT IT -- i.e. IT CONFIGURED THE SAI AS A *TARGET*,
 * WHERE THE BIT CLOCK COMES FROM AN EXTERNAL CODEC -- AND THEN ASSERTED THE CONTROLLER'S
 * DIVIDER MATH.  The model applied the divider unconditionally, so BOTH HALVES OF THE
 * LOOP SHARED THE SAME WRONG BELIEF, and the test was green for it.
 *
 * On this board the SAI drives a TXD->RXD jumper: there is no codec, so THE SAI IS THE
 * CONTROLLER -- nothing else could possibly generate the clock.  Say so.
 *
 * (93emulator: "CHECK BCD BEFORE YOU WRITE ONE LINE OF DIVIDER MATH.  IF IT IS 0, THE
 *  ANSWER IS NOT IN THIS DEVICE." -- their SAI is a bit-clock slave and their RM-correct
 *  divider formula was a fabrication with a citation attached.)
 */
#define TCR2_BCD  (1u << 24)   /* generate the bit clock internally: we are the controller */
#define SAI_TCR5 (*(volatile uint32_t *)(SAI0 + 0x1C))
#define SAI_TDR0_ADDR (SAI0 + 0x20)
#define SAI_TFR0 (*(volatile uint32_t *)(SAI0 + 0x40))
#define SAI_RCSR (*(volatile uint32_t *)(SAI0 + 0x88))
#define SAI_RCR1 (*(volatile uint32_t *)(SAI0 + 0x8C))
#define SAI_RCR5 (*(volatile uint32_t *)(SAI0 + 0x9C))
#define SAI_RDR0 (*(volatile uint32_t *)(SAI0 + 0xA0))
#define SAI_RFR0 (*(volatile uint32_t *)(SAI0 + 0xC0))

#define CSR_FRDE (1u << 0)    /* FIFO request DMA enable */
#define CSR_FR   (1u << 25)   /* FIFO reset */
#define CSR_EN   (1u << 31)   /* TE / RE */
#define FR_COUNT(v) (((v) >> 16) & 0xF)

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

#define CSR_ERQ      (1u << 0)
#define CSR_DONE     (1u << 30)
#define TCD_INTMAJOR (1u << 1)
#define TCD_DREQ     (1u << 3)
#define ATTR_32BIT   ((2u << 8) | 2u)   /* SSIZE=2 (4 bytes), DSIZE=2 */

/* SAI0 Tx request-mux source (CMSIS dma_request_source_t). */
#define DMAREQ_SAI0_TX 100

#define NSAMPLES 8
#define CHAN 0

/* Zero-init and filled at run time: there is no startup .data copy (the link
 * script asserts it), so an initialised global would sit in flash. */
static volatile uint32_t samples[NSAMPLES];

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    int i;
    int got = 0;
    uint32_t rx[NSAMPLES];

    LP_CTRL = CTRL_TE_U;
    puts_("SAI-DMA test\r\n");

    /* The SAI MCLK is now DERIVED from SYSCON SAI0CLKSEL -- point it at FRO_HF (48 MHz, live
     * out of reset) so the SAI has a bit clock.  This test checks byte-exactness, not rate,
     * so any non-zero source works (CLOCK_AttachClk(kFRO_HF_to_SAI0)). */
    *(volatile uint32_t *)0x40000888u = 0;      /* SAI0CLKDIV = 0 */
    *(volatile uint32_t *)0x40000880u = 3u;     /* SAI0CLKSEL = FRO_HF */

    /* Audio words nothing in the model could invent. */
    samples[0] = 0x11111111u; samples[1] = 0x22222222u;
    samples[2] = 0x33333333u; samples[3] = 0x44444444u;
    samples[4] = 0xDEADBEEFu; samples[5] = 0xCAFEBABEu;
    samples[6] = 0x0BADF00Du; samples[7] = 0xFEEDFACEu;

    /* --- SAI: 32-bit words, fastest bit clock, TX FIFO asks at half ------- */
    SAI_TCR2 = TCR2_BCD;                  /* DIV = 0                                */
    SAI_TCR5 = (31u << 16);        /* 32-bit words                           */
    SAI_RCR5 = (31u << 16);
    SAI_TCR1 = 4;                  /* TFW = 4: ask for data at the halfway mark */
    SAI_RCR1 = 0;                  /* RFW = 0: data ready on the first word  */
    SAI_TCSR = CSR_FR;             /* reset both FIFOs                       */
    SAI_RCSR = CSR_FR;
    ok &= (FR_COUNT(SAI_TFR0) == 0);

    /* --- eDMA channel: memory -> SAI TDR, one 32-bit word per request ----- */
    TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)samples;
    TCD_SOFF(CHAN)   = 4;                    /* walk the source buffer       */
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 4;                    /* ONE WORD per DMA request     */
    TCD_SLAST(CHAN)  = (uint32_t)(-(int32_t)(4 * NSAMPLES));
    TCD_DADDR(CHAN)  = SAI_TDR0_ADDR;        /* the FIFO does not advance    */
    TCD_DOFF(CHAN)   = 0;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = NSAMPLES;
    TCD_BITER(CHAN)  = NSAMPLES;
    TCD_CSR(CHAN)    = TCD_INTMAJOR | TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_SAI0_TX;       /* listen to the SAI's TX line  */

    /* Nothing has moved yet: no request has been raised. */
    ok &= (FR_COUNT(SAI_TFR0) == 0);

    /* --- go: enable the receiver, the DMA request, then the transmitter --- */
    SAI_RCSR = CSR_EN;                       /* receiver on (loopback wire)  */
    CH_CSR(CHAN) = CSR_ERQ;                  /* HARDWARE requests enabled    */

    /*
     * Arming FRDE is the moment the SAI starts asking.  Its FIFO is empty, so
     * it asserts immediately and the eDMA fills it — WITHOUT THE CPU.
     */
    SAI_TCSR = CSR_EN | CSR_FRDE;

    /* The DMA is ASYNCHRONOUS (it runs in a bottom half, as real DMA takes
     * time) -- the FIFO does not fill on this very instruction.  What matters
     * is that the words arrive at all, and that the CPU never wrote them. */

    /* --- let the word clock drain the FIFO; the DMA keeps refilling ------- */
    for (d = 0; d < 20000000 && got < NSAMPLES; d++) {
        if (FR_COUNT(SAI_RFR0) > 0) {
            rx[got++] = SAI_RDR0;
        }
    }
    ok &= (got == NSAMPLES);

    /* Byte-exact and in order — carried entirely by the DMA. */
    for (i = 0; i < got; i++) {
        ok &= (rx[i] == samples[i]);
    }

    /* --- the major loop finished, and DREQ stopped the channel ------------ */
    ok &= !!(CH_CSR(CHAN) & CSR_DONE);       /* major loop complete          */
    ok &= !(CH_CSR(CHAN) & CSR_ERQ);         /* DREQ auto-cleared the request */
    ok &= !!(CH_INT(CHAN) & 1u);             /* INTMAJOR latched             */

    SAI_TCSR = 0;
    puts_(ok ? "SAIDMA PASS\r\n" : "SAIDMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
