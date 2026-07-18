/*
 * MCXN947 FlexSPI <-> eDMA data path (peripheral-triggered, stock-driver-shaped).
 *
 * A DMA-driven FlexSPI driver (FLEXSPI_TransferEDMA) arms an eDMA channel at TFDR/RFDR,
 * sets IP{TX,RX}FCR[DMAEN], and NEVER touches the data registers itself -- every byte of a
 * program or a read must be carried by the DMA on the FlexSPI TX (mux src 2) / RX (src 1)
 * request lines.  Before those lines were wired, {TX,RX}DMAEN drove nothing: the FlexSPI
 * raised no request, the channel (ERQ set) waited forever, and a DMA-driven flash transfer
 * hung.
 *
 * Round trip, both directions in one run, against a golden the FlexSPI model does not own:
 *   TX-DMA page-programs a known pattern into a genuine m25p80 NOR, then
 *   RX-DMA reads it back and demands it byte-exact.
 * A model that raised neither request moves zero bytes and fails.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP_CTRL (*(volatile uint32_t *)0x400B4018u)
#define LP_STAT (*(volatile uint32_t *)0x400B4014u)
#define LP_DATA (*(volatile uint32_t *)0x400B401Cu)
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

/* ---- FlexSPI0 ------------------------------------------------------------- */
#define FSPI 0x400C8000u
#define FSPI_INTR     (*(volatile uint32_t *)(FSPI + 0x14))
#define FSPI_IPCR0    (*(volatile uint32_t *)(FSPI + 0xA0))
#define FSPI_IPCR1    (*(volatile uint32_t *)(FSPI + 0xA4))
#define FSPI_IPCMD    (*(volatile uint32_t *)(FSPI + 0xB0))
#define FSPI_IPRXFCR  (*(volatile uint32_t *)(FSPI + 0xB8))
#define FSPI_IPTXFCR  (*(volatile uint32_t *)(FSPI + 0xBC))
#define FSPI_LUT(i)   (*(volatile uint32_t *)(FSPI + 0x200 + (i) * 4))
#define FSPI_RFDR0_ADDR 0x400C8100u
#define FSPI_TFDR0_ADDR 0x400C8180u

#define INTR_IPCMDDONE (1u << 0)
#define INTR_IPCMDERR  (1u << 3)
#define IPCMD_TRG      (1u << 0)
#define FCR_CLR        (1u << 0)
#define FCR_DMAEN      (1u << 1)

#define OP_STOP  0x00
#define OP_CMD   0x01
#define OP_RADDR 0x02
#define OP_WRITE 0x08
#define OP_READ  0x09
#define PAD1     0
#define LUT_SEQ(c0, p0, o0, c1, p1, o1) \
    ((uint32_t)(o0) | ((uint32_t)(p0) << 8) | ((uint32_t)(c0) << 10) | \
     ((uint32_t)(o1) << 16) | ((uint32_t)(p1) << 24) | ((uint32_t)(c1) << 26))
#define SEQ_READ 0
#define SEQ_WREN 1
#define SEQ_SE   2
#define SEQ_PP   3
#define TEST_OFF 0x00100000u

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
#define DMAREQ_FLEXSPI0_RX 1u
#define DMAREQ_FLEXSPI0_TX 2u
#define CHAN 0
#define NW   4

static void ip_run(uint32_t seq, uint32_t addr, uint32_t len)
{
    FSPI_IPRXFCR = FCR_CLR;
    FSPI_IPTXFCR = FCR_CLR;
    FSPI_INTR = INTR_IPCMDDONE | INTR_IPCMDERR;
    FSPI_IPCR0 = addr;
    FSPI_IPCR1 = len | (seq << 16);
    FSPI_IPCMD = IPCMD_TRG;
}
static int ip_wait(void)
{
    /* Bounded: if a DMA phase failed to feed/drain, the command never completes -- return
     * failure instead of hanging, so the test prints a clean FAIL rather than timing out. */
    int g = 20000000;

    while (!(FSPI_INTR & INTR_IPCMDDONE) && g--) {
    }
    return (FSPI_INTR & INTR_IPCMDDONE) && !(FSPI_INTR & INTR_IPCMDERR);
}
static int nor_wren(void) { ip_run(SEQ_WREN, 0, 0); return ip_wait(); }

/* Arm one channel: <src> -> <dst>, one 32-bit word per request, <n> words. */
static void dma_arm(uint32_t src, uint16_t soff, uint32_t dst, uint16_t doff,
                    uint32_t mux, uint32_t n)
{
    CH_CSR(CHAN)     = CSR_DONE;      /* W1C any stale DONE from a prior transfer */
    CH_INT(CHAN)     = 1u;            /* W1C any stale major-loop interrupt        */
    TCD_SADDR(CHAN)  = src;
    TCD_SOFF(CHAN)   = soff;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = 4;
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = dst;
    TCD_DOFF(CHAN)   = doff;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = n;
    TCD_BITER(CHAN)  = n;
    TCD_CSR(CHAN)    = TCD_INTMAJOR | TCD_DREQ;
    CH_MUX(CHAN)     = mux;
    CH_CSR(CHAN)     = CSR_ERQ;
}

static int dma_done(void)
{
    volatile int d;

    for (d = 0; d < 20000000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }
    return (CH_CSR(CHAN) & CSR_DONE) && !(CH_CSR(CHAN) & CSR_ERQ) && (CH_INT(CHAN) & 1u);
}

static const uint32_t pat[NW] = { 0xC0DED00Du, 0x0BADF00Du, 0xFEEDFACEu, 0x8BADF00Du };
static volatile uint32_t out[NW];

void cpu0_main(void)
{
    int ok = 1;
    int i;

    LP_CTRL = (1u << 19);
    puts_("FLEXSPI-DMA test\r\n");

    FSPI_LUT(SEQ_READ * 4)     = LUT_SEQ(OP_CMD, PAD1, 0x03, OP_RADDR, PAD1, 24);
    FSPI_LUT(SEQ_READ * 4 + 1) = LUT_SEQ(OP_READ, PAD1, 0x04, OP_STOP, PAD1, 0);
    FSPI_LUT(SEQ_WREN * 4)     = LUT_SEQ(OP_CMD, PAD1, 0x06, OP_STOP, PAD1, 0);
    FSPI_LUT(SEQ_SE * 4)       = LUT_SEQ(OP_CMD, PAD1, 0x20, OP_RADDR, PAD1, 24);
    FSPI_LUT(SEQ_SE * 4 + 1)   = LUT_SEQ(OP_STOP, PAD1, 0, OP_STOP, PAD1, 0);
    FSPI_LUT(SEQ_PP * 4)       = LUT_SEQ(OP_CMD, PAD1, 0x02, OP_RADDR, PAD1, 24);
    FSPI_LUT(SEQ_PP * 4 + 1)   = LUT_SEQ(OP_WRITE, PAD1, 0x04, OP_STOP, PAD1, 0);

    /* --- erase the sector (CPU) ------------------------------------------- */
    ok &= nor_wren();
    ip_run(SEQ_SE, TEST_OFF, 0);
    ok &= ip_wait();

    /* --- TX-DMA: the eDMA feeds TFDR the program data (CPU never writes it) - */
    ok &= nor_wren();
    dma_arm((uint32_t)(uintptr_t)pat, 4, FSPI_TFDR0_ADDR, 0, DMAREQ_FLEXSPI0_TX, NW);
    FSPI_IPRXFCR = FCR_CLR;
    FSPI_IPTXFCR = FCR_CLR | FCR_DMAEN;          /* arm TXDMAEN */
    FSPI_INTR = INTR_IPCMDDONE | INTR_IPCMDERR;
    FSPI_IPCR0 = TEST_OFF;
    FSPI_IPCR1 = sizeof(pat) | (SEQ_PP << 16);
    FSPI_IPCMD = IPCMD_TRG;                       /* IPTXWE asserts -> DMA feeds TFDR */
    ok &= dma_done();
    ok &= ip_wait();

    /* --- RX-DMA: the eDMA drains RFDR into memory (CPU never reads it) ----- */
    dma_arm(FSPI_RFDR0_ADDR, 0, (uint32_t)(uintptr_t)out, 4, DMAREQ_FLEXSPI0_RX, NW);
    ok &= (out[0] == 0);                          /* nothing moved yet */
    FSPI_IPRXFCR = FCR_CLR | FCR_DMAEN;           /* arm RXDMAEN */
    FSPI_IPTXFCR = FCR_CLR;
    FSPI_INTR = INTR_IPCMDDONE | INTR_IPCMDERR;
    FSPI_IPCR0 = TEST_OFF;
    FSPI_IPCR1 = sizeof(pat) | (SEQ_READ << 16);
    FSPI_IPCMD = IPCMD_TRG;                        /* RX FIFO fills -> DMA drains it */
    ok &= dma_done();

    /* --- the round trip: DMA-programmed and DMA-read, byte-exact ---------- */
    for (i = 0; i < NW; i++) {
        ok &= (out[i] == pat[i]);
    }

    puts_(ok ? "FLEXSPI-DMA PASS\r\n" : "FLEXSPI-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
