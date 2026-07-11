/*
 * MCXN947 DAC output-FIFO test.
 *
 * Exercises the DAC's observable digital contract for real: occupancy, the
 * FULL/EMPTY/watermark flags, the read/write pointers, overflow on a write to a
 * full FIFO, underflow on a trigger against an empty one, and — the important
 * one — that a level-triggered FIFO interrupt DEASSERTS once the ISR refills.
 *
 * The analog voltage is the only thing firmware cannot observe, so it is the
 * only thing not checked here.
 *
 * Each check corresponds to a silent-wrong-answer in the old register-file
 * model, which had no FIFO and reported FSR = EMPTY|WM forever:
 *
 *   NEG1  writing 17 samples into DAC0's 16-deep FIFO must set FSR[OF] and drop
 *         the 17th — the write pointer must NOT advance.  (The old model
 *         accepted an unbounded burst and still claimed the FIFO was empty.)
 *   NEG2  triggering an empty FIFO must set FSR[UF].
 *   NEG3  the ISR must be able to clear the request by writing DATA alone.  The
 *         old model pinned EMPTY high, so a level-triggered EMPTY_IE re-entered
 *         for ever; the old version of this very test had to disable IER inside
 *         its own handler to survive, which is what made it pass.
 *   NEG4  PARAM must report each instance's real FIFO depth: DAC0/1 are 16
 *         (FIFOSZ=3), DAC2 is 32 (FIFOSZ=4).  The old model said 32 for all
 *         three, so a driver sizing its FIFO from PARAM overruns DAC0/1 by 2x.
 *
 * Prints "DAC PASS" only if every check holds.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

#define DAC0 0x4010F000u
#define DAC2 0x40114000u
#define R_PARAM 0x04
#define R_DATA  0x08
#define R_GCR   0x0C
#define R_FCR   0x10
#define R_FPR   0x14
#define R_FSR   0x18
#define R_IER   0x1C
#define R_RCR   0x24
#define R_TCR   0x28
#define REG(b, r) (*(volatile uint32_t *)((b) + (r)))

/* GCR */
#define GCR_DACEN  (1u << 0)
#define GCR_FIFOEN (1u << 3)
/* FSR */
#define FSR_FULL   (1u << 0)
#define FSR_EMPTY  (1u << 1)
#define FSR_WM     (1u << 2)
#define FSR_OF     (1u << 6)
#define FSR_UF     (1u << 7)
/* IER (bits line up with FSR) */
#define IER_EMPTY_IE (1u << 1)
/* TCR / RCR */
#define TCR_SWTRG   (1u << 0)
#define RCR_FIFORST (1u << 1)

#define DAC0_DEPTH 16
#define DAC0_IRQ 106
#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)  /* IRQ 96..127 */

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

static volatile uint32_t irq_count;
static volatile uint32_t refills;

/*
 * A DAC driver's real ISR: it refills the FIFO and does NOT touch IER.  The
 * request must clear because writing DATA clears FSR[EMPTY].  If EMPTY were
 * pinned high (as the bring-up model did), this would re-enter for ever.
 */
void dac0_handler(void)
{
    irq_count++;
    refills++;
    REG(DAC0, R_DATA) = 0x123;     /* refill -> EMPTY clears -> IRQ deasserts */
}

#define FPR_RPT(v) ((v) & 0xF)
#define FPR_WPT(v) (((v) >> 16) & 0xF)

void cpu0_main(void)
{
    int ok = 1;
    uint32_t fpr;

    LP_CTRL = CTRL_TE;
    puts_("DAC test\r\n");

    /* --- NEG4: PARAM must describe each instance's real geometry --------- */
    ok &= (REG(DAC0, R_PARAM) == 3);   /* depth 2^(3+1) = 16 */
    ok &= (REG(DAC2, R_PARAM) == 4);   /* depth 2^(4+1) = 32 */

    /* --- enable DAC0 with its FIFO, watermark level 0 -------------------- */
    REG(DAC0, R_FCR) = 0;                          /* WML = 0 */
    REG(DAC0, R_GCR) = GCR_DACEN | GCR_FIFOEN;

    /* Empty FIFO: EMPTY and WM set, FULL clear. */
    ok &= !!(REG(DAC0, R_FSR) & FSR_EMPTY);
    ok &=  !(REG(DAC0, R_FSR) & FSR_FULL);
    ok &= !!(REG(DAC0, R_FSR) & FSR_WM);
    ok &= (REG(DAC0, R_FPR) == 0);

    /* --- push one sample: EMPTY clears, the write pointer advances -------- */
    REG(DAC0, R_DATA) = 0x111;
    ok &= !(REG(DAC0, R_FSR) & FSR_EMPTY);
    fpr = REG(DAC0, R_FPR);
    ok &= (FPR_WPT(fpr) == 1 && FPR_RPT(fpr) == 0);

    /* --- fill it to the brim: FULL sets ---------------------------------- */
    for (int i = 1; i < DAC0_DEPTH; i++) {
        REG(DAC0, R_DATA) = 0x200 + i;
    }
    ok &= !!(REG(DAC0, R_FSR) & FSR_FULL);
    ok &=  !(REG(DAC0, R_FSR) & FSR_EMPTY);
    ok &=  !(REG(DAC0, R_FSR) & FSR_OF);      /* not overflowed yet */

    /* --- NEG1: one more write overflows and is DROPPED -------------------- */
    fpr = REG(DAC0, R_FPR);
    REG(DAC0, R_DATA) = 0xFFF;
    ok &= !!(REG(DAC0, R_FSR) & FSR_OF);      /* overflow flagged */
    ok &= (REG(DAC0, R_FPR) == fpr);          /* write pointer did NOT advance */
    REG(DAC0, R_FSR) = FSR_OF;                /* W1C */
    ok &= !(REG(DAC0, R_FSR) & FSR_OF);

    /* --- triggers pop samples: FULL clears, then EMPTY returns ------------ */
    REG(DAC0, R_TCR) = TCR_SWTRG;
    ok &= !(REG(DAC0, R_FSR) & FSR_FULL);
    fpr = REG(DAC0, R_FPR);
    ok &= (FPR_RPT(fpr) == 1);                /* read pointer advanced */

    for (int i = 1; i < DAC0_DEPTH; i++) {
        REG(DAC0, R_TCR) = TCR_SWTRG;
    }
    ok &= !!(REG(DAC0, R_FSR) & FSR_EMPTY);   /* fully drained */

    /* --- NEG2: triggering an empty FIFO underflows ------------------------ */
    ok &= !(REG(DAC0, R_FSR) & FSR_UF);
    REG(DAC0, R_TCR) = TCR_SWTRG;
    ok &= !!(REG(DAC0, R_FSR) & FSR_UF);
    REG(DAC0, R_FSR) = FSR_UF;                /* W1C */

    /* --- NEG3: a level-triggered EMPTY interrupt must deassert ------------ */
    REG(DAC0, R_RCR) = RCR_FIFORST;           /* empty, flags clear */
    irq_count = 0;
    refills = 0;

    NVIC_ISER3 = (1u << (DAC0_IRQ - 96));
    __asm__ volatile ("cpsie i");

    REG(DAC0, R_IER) = IER_EMPTY_IE;          /* FIFO is empty -> request */

    while (refills < 1) {
    }
    /* The ISR refilled without touching IER.  Give any storm plenty of room to
     * show itself, then require that exactly one interrupt was taken. */
    for (volatile int i = 0; i < 200000; i++) {
    }
    ok &= (irq_count == 1);                   /* deasserted; no ISR storm */

    REG(DAC0, R_IER) = 0;
    __asm__ volatile ("cpsid i");

    puts_(ok ? "DAC PASS\r\n" : "DAC FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[130] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + DAC0_IRQ] = dac0_handler,  /* exception 122 = IRQ 106 */
};
