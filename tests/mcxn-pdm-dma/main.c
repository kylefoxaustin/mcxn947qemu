/*
 * MCXN947 PDM/MICFIL FIFO -> eDMA (operator-fed microphone capture, stock-driver-shaped).
 *
 * A PDM microphone bitstream has no source in emulation, so the samples are OPERATOR-FED:
 * the harness pushes 24-bit PCM samples into channel 0 over QMP ("mic-input" QOM property),
 * exactly as a board-farm audio source would.  With CTRL_1[DISEL]=DMA, each sample past the
 * watermark asserts the MICFIL FIFO request (CMSIS source 18 -- a FIFO LEVEL, like the SAI),
 * and the eDMA drains DATACH0 into memory.  Before this line was wired, DISEL=DMA drove
 * nothing: the FIFO stayed empty (no source), the request never asserted, and MICFIL DMA
 * capture blocked forever.
 *
 * Oracle the model does not own: the harness feeds a KNOWN sample sequence and the guest
 * demands that exact sequence back from memory, carried entirely by the DMA (the CPU never
 * reads DATACH0).  A dead request line moves nothing; wrong-order or corrupt draining fails
 * the byte-exact compare.
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

/* ---- PDM/MICFIL0 --------------------------------------------------------- */
#define PDM0 0x4010C000u
#define PDM_CTRL_1    (*(volatile uint32_t *)(PDM0 + 0x00))
#define PDM_FIFOC     (*(volatile uint32_t *)(PDM0 + 0x10))
#define PDM_DATACH0_ADDR (PDM0 + 0x24)
#define CTRL1_CH0EN     (1u << 0)
#define CTRL1_DISEL_DMA (1u << 24)
#define CTRL1_SRES      (1u << 27)
#define CTRL1_PDMIEN    (1u << 29)

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
#define DMAREQ_MICFIL0 18u
#define CHAN 0
#define NW   8

/* The operator (harness) pushes exactly these 24-bit samples; the DMA must land
 * them, in order, in out[].  Shared constant: the OPERATOR is the data source, not
 * the model -- the model only conveys mic -> FIFO -> DMA -> memory. */
static const uint32_t mic[NW] = {
    0x112233u, 0x445566u, 0x778899u, 0xAABBCCu,
    0x0D1E2Fu, 0x334455u, 0x667788u, 0x99AABBu,
};
static volatile uint32_t out[NW];

void cpu0_main(void)
{
    int ok = 1, i, g;

    LP_CTRL = (1u << 19);
    puts_("PDM-DMA test\r\n");

    for (i = 0; i < NW; i++) {
        out[i] = 0xDEAD0000u + i;        /* sentinels: the CPU's only writes to out[] */
    }

    /* Reset then enable MICFIL: channel 0, filter on, DMA request select, watermark 0
     * (so any queued sample asserts the request and the eDMA drains the FIFO fully). */
    PDM_CTRL_1 = CTRL1_SRES;
    PDM_FIFOC = 0;                       /* watermark = 0 */
    PDM_CTRL_1 = CTRL1_PDMIEN | CTRL1_CH0EN | CTRL1_DISEL_DMA;

    /* Arm ch0: DATACH0 -> out[], one 32-bit sample per FIFO request, NW samples. */
    CH_CSR(CHAN)     = CSR_DONE;         /* W1C stale DONE */
    CH_INT(CHAN)     = 1u;
    TCD_SADDR(CHAN)  = PDM_DATACH0_ADDR;
    TCD_SOFF(CHAN)   = 0;                /* source fixed on DATACH0 */
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 4;
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)out;
    TCD_DOFF(CHAN)   = 4;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = NW;
    TCD_BITER(CHAN)  = NW;
    TCD_CSR(CHAN)    = TCD_INTMAJOR | TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_MICFIL0;
    CH_CSR(CHAN)     = CSR_ERQ;

    puts_("PDM-DMA ARMED\r\n");          /* harness now pushes NW mic samples */

    /* Wait (bounded) for the whole capture; a dead request line never sets DONE. */
    g = 200000000;
    while (!(CH_CSR(CHAN) & CSR_DONE) && g--) {
    }

    ok &= (CH_CSR(CHAN) & CSR_DONE) && !(CH_CSR(CHAN) & CSR_ERQ) && (CH_INT(CHAN) & 1u);
    for (i = 0; i < NW; i++) {
        ok &= (out[i] == mic[i]);        /* byte-exact operator samples, in order */
    }

    puts_(ok ? "PDM-DMA PASS\r\n" : "PDM-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
