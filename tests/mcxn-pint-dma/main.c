/*
 * MCXN947 PINT (pin interrupt) edge -> eDMA + NVIC (operator-driven, stock-driver-shaped).
 *
 * A PINT channel edge drives an eDMA request line (CMSIS PINT INT0..3 = sources 3..6) and,
 * on the shared line PINT0_IRQn=47, an NVIC interrupt.  A pin edge is analog-ish and has no
 * source in emulation, so the 8 channel input levels are OPERATOR-DRIVEN: the harness sets
 * the "pin-input" QOM property, exactly as a board-farm control plane would drive the pins.
 * Before PINT was functional, IST/RISE always read 0, no IRQ fired, and DMA could not pace.
 *
 * One rising edge on channel 0 validates the whole new path at once:
 *   DATA  -- out[0] holds the first pattern word, carried there by the edge's DMA (CPU never
 *            writes out[]); a dead request line leaves the sentinel.
 *   RATE  -- out[1] is still the sentinel and CITER == NW-1: exactly one minor loop ran.
 *   EDGE  -- RISE[0] and IST[0] latched (the edge-detect that feeds both IRQ and DMA).
 *   NVIC  -- with IST[0] still pending, enabling IRQ 47 fires the handler (interrupt delivery).
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

/* ---- PINT0 --------------------------------------------------------------- */
#define PINT0 0x40004000u
#define PINT_ISEL  (*(volatile uint32_t *)(PINT0 + 0x00))
#define PINT_SIENR (*(volatile uint32_t *)(PINT0 + 0x08))   /* set IENR (rising enable) */
#define PINT_RISE  (*(volatile uint32_t *)(PINT0 + 0x1C))
#define PINT_IST   (*(volatile uint32_t *)(PINT0 + 0x24))

/* ---- NVIC (IRQ 47 = PINT0) ----------------------------------------------- */
#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)      /* IRQ 32..63 */
#define PINT0_IRQ 47

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
#define DMAREQ_PINT_INT0 3u
#define CHAN 0
#define NW   4
#define SENT 0xEEEE0000u

static const uint32_t pat[NW] = { 0xC0DED00Du, 0x0BADF00Du, 0xFEEDFACEu, 0x8BADF00Du };
static volatile uint32_t out[NW];
static volatile int pint_fired;

void pint0_handler(void)
{
    PINT_IST = 0xFFu;          /* W1C: clear the pending status -> drops the NVIC line */
    pint_fired = 1;
}

void cpu0_main(void)
{
    int ok = 1, i, g;

    LP_CTRL = (1u << 19);
    puts_("PINT-DMA test\r\n");

    for (i = 0; i < NW; i++) {
        out[i] = SENT + i;               /* sentinels: the CPU's only writes to out[] */
    }

    /* Arm ch0: pat[] -> out[], one 32-bit word per PINT INT0 edge, NW words.  No DREQ:
     * the channel stays armed so a broken auto-ack (whole buffer on one edge) shows. */
    CH_CSR(CHAN)     = (1u << 30);        /* W1C stale DONE */
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
    CH_MUX(CHAN)     = DMAREQ_PINT_INT0;
    CH_CSR(CHAN)     = CSR_ERQ;

    /* Channel 0: edge mode, rising-edge enabled.  NVIC left disabled for now. */
    PINT_ISEL  = 0;
    PINT_SIENR = 1u;                      /* enable rising edge on channel 0 */

    puts_("PINT-DMA ARMED\r\n");          /* harness now injects pin-input=1 (ch0 rising) */

    /* One edge must move exactly ONE word.  Wait (bounded) for word 0 to land. */
    g = 200000000;
    while (out[0] == (SENT + 0) && g--) {
    }

    ok &= (out[0] == pat[0]);             /* DATA: the edge's DMA carried word 0        */
    ok &= (out[1] == (SENT + 1));         /* RATE: word 1 did NOT move -- one per edge   */
    ok &= (TCD_CITER(CHAN) == NW - 1);    /* exactly one minor loop consumed             */
    ok &= (PINT_RISE & 1u);               /* EDGE: rising-edge flag latched              */
    ok &= (PINT_IST & 1u);                /* EDGE: interrupt status latched              */

    /* NVIC delivery: IST[0] is still pending, so enabling IRQ 47 fires the handler. */
    NVIC_ISER1 = (1u << (PINT0_IRQ - 32));
    __asm__ volatile ("cpsie i");
    g = 20000000;
    while (!pint_fired && g--) {
    }
    ok &= (pint_fired == 1);              /* NVIC: the shared PINT interrupt was delivered */

    puts_(ok ? "PINT-DMA PASS\r\n" : "PINT-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[128] = {
    [0]  = (vec_t)0x20010000u,
    [1]  = cpu0_main,
    [16 + PINT0_IRQ] = pint0_handler,     /* exception 63 = IRQ 47 */
};
