/*
 * MCXN947 CMP (analog comparator) crossing -> eDMA (operator-driven, stock-driver-shaped).
 *
 * When CCR1[DMA_EN] is set, an IER-enabled comparator edge "forces a DMA transfer request
 * rather than a CPU interrupt" (RM / fsl_lpcmp LPCMP_EnableDMA).  The crossing itself is
 * analog and has no source in emulation, so it is OPERATOR-DRIVEN: the harness injects the
 * comparator output level over QMP ("comparator-output"), exactly as a board-farm control
 * plane would drive the +/- inputs.  The crossing (mux source HsCmp0 = 28) then paces the
 * eDMA.  Before this line was wired, DMA_EN drove nothing and a comparator-paced DMA hung.
 *
 * The oracle proves ONE crossing moves EXACTLY ONE word -- both axes in a single injection:
 *   DATA -- out[0] must hold the first pattern word, carried there by the crossing's DMA
 *           (the CPU never writes out[]); a dead request line leaves the sentinel.
 *   RATE -- out[1] must still be the sentinel and CITER == NW-1: exactly one minor loop ran.
 *           A model that drained the whole buffer on one crossing writes out[1] too.
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

/* ---- CMP0 ---------------------------------------------------------------- */
#define CMP0 0x40051000u
#define CMP_CCR0 (*(volatile uint32_t *)(CMP0 + 0x08))
#define CMP_CCR1 (*(volatile uint32_t *)(CMP0 + 0x0C))
#define CMP_IER  (*(volatile uint32_t *)(CMP0 + 0x1C))
#define CCR0_CMP_EN  (1u << 0)
#define CCR1_DMA_EN  (1u << 2)
#define IER_CFR_IE   (1u << 0)

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
#define TCD_INTMAJOR (1u << 1)
#define DMAREQ_HSCMP0 28u
#define CHAN 0
#define NW   4
#define SENT 0xEEEE0000u

static const uint32_t pat[NW] = { 0xC0DED00Du, 0x0BADF00Du, 0xFEEDFACEu, 0x8BADF00Du };
static volatile uint32_t out[NW];

void cpu0_main(void)
{
    int ok = 1, i, g;

    LP_CTRL = (1u << 19);
    puts_("CMP-DMA test\r\n");

    for (i = 0; i < NW; i++) {
        out[i] = SENT + i;               /* sentinels: the CPU's only writes to out[] */
    }

    /* Arm ch0: pat[] -> out[], one 32-bit word per comparator crossing, NW words.
     * NOTE: no TCD_DREQ -- we want the channel to stay armed across crossings so a
     * broken auto-ack (whole buffer on one crossing) is observable, not hidden by DONE. */
    CH_CSR(CHAN)     = (1u << 30);        /* W1C any stale DONE */
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
    TCD_CSR(CHAN)    = TCD_INTMAJOR;
    CH_MUX(CHAN)     = DMAREQ_HSCMP0;
    CH_CSR(CHAN)     = CSR_ERQ;

    /* Enable the comparator and route an IER-enabled rising edge to DMA (not the NVIC). */
    CMP_CCR0 = CCR0_CMP_EN;
    CMP_CCR1 = CCR1_DMA_EN;
    CMP_IER  = IER_CFR_IE;

    puts_("CMP-DMA ARMED\r\n");           /* harness now injects comparator-output=true */

    /* One crossing must move exactly ONE word.  Wait (bounded) for word 0 to land. */
    g = 200000000;
    while (out[0] == (SENT + 0) && g--) {
    }

    ok &= (out[0] == pat[0]);             /* DATA: the crossing's DMA carried word 0    */
    ok &= (out[1] == (SENT + 1));         /* RATE: word 1 did NOT move -- one per crossing */
    ok &= (TCD_CITER(CHAN) == NW - 1);    /* exactly one minor loop consumed             */

    puts_(ok ? "CMP-DMA PASS\r\n" : "CMP-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
