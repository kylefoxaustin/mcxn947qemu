/*
 * "OUTPUT A WAVEFORM ON A TIMER" -- the DAC's hardware-trigger path, the counterpart of
 * the ADC's "sample on a trigger":
 *
 *     CTIMER0 match-3  ->  INPUTMUX (selector 5 -> DAC0_TRIG)
 *                      ->  DAC0 hardware trigger (GCR[TRGSEL]=0, hardware mode)
 *                      ->  pop one FIFO sample to the analog output.
 *
 * A field/waveform DAC loop fills the output FIFO once and lets a timer pace each sample
 * out -- zero CPU involvement per sample (the standard fsl_dac external-trigger use).  It
 * was impossible: DAC0_TRIG was a stored INPUTMUX selector that reached NOTHING, and the
 * DAC only advanced on TCR[SWTRG].  A guest configuring hardware-triggered output got a
 * timer that ticked and a DAC that never advanced -- silent.
 *
 * The test fills the FIFO with 4 samples, runs CTIMER0 match-3 continuously, and checks
 * the FIFO pointer (FPR) + status (FSR) from the GUEST:
 *   PHASE 1 (GCR[TRGSEL]=1, SOFTWARE mode): the routed hardware trigger MUST be ignored
 *           -- the read pointer stays put, the FIFO does not drain.
 *   PHASE 2 (GCR[TRGSEL]=0, HARDWARE mode): each match pops one sample; the FIFO drains
 *           to EMPTY and the next match UNDERFLOWS (FSR[UF]) -- so the timer really is
 *           advancing the output.
 *
 * Selector 5 = CTIMER0 match-3, DERIVED from NXP's driver (kINPUTMUX_Ctimer0M3ToDac0
 * Trigger = 5).  The CPU never writes TCR[SWTRG].
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4 + 0x1C))
#define CTRL_TE   (1u << 19)
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

static void puthex(uint32_t v)
{
    const char *H = "0123456789ABCDEF";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        putc_(H[(v >> i) & 0xF]);
    }
}

/* ---- clocks (CTIMER0 <- FRO_HF 48 MHz) ----------------------------------- */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define FIRCCSR_FIRCEN (1u << 0)
#define CTIMER0CLKSEL (*(volatile uint32_t *)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t *)0x400003D0u)
#define SEL_FROHF   3u

/* ---- CTIMER0 (base 0x4000C000) ------------------------------------------- */
#define CT0 0x4000C000u
#define CT0_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT0_MCR (*(volatile uint32_t *)(CT0 + 0x14))
#define CT0_MR3 (*(volatile uint32_t *)(CT0 + 0x18 + 3 * 4))
#define TCR_RUN 1u
#define TCR_RST 2u
#define MCR_RST3 (1u << 10)          /* reset TC on match 3 -> periodic M3 */
#define MR3_COUNTS 300u

/* ---- DAC0 (base 0x4010F000) ---------------------------------------------- */
#define DAC0 0x4010F000u
#define DAC_DATA (*(volatile uint32_t *)(DAC0 + 0x08))   /* WO */
#define DAC_GCR  (*(volatile uint32_t *)(DAC0 + 0x0C))
#define DAC_FPR  (*(volatile uint32_t *)(DAC0 + 0x14))   /* RO: wptr<<16 | rptr */
#define DAC_FSR  (*(volatile uint32_t *)(DAC0 + 0x18))
#define GCR_DACEN  (1u << 0)
#define GCR_FIFOEN (1u << 3)
#define GCR_TRGSEL (1u << 5)         /* 0 = hardware trigger, 1 = software trigger */
#define FSR_EMPTY  (1u << 1)
#define FSR_UF     (1u << 7)

/* INPUTMUX0: DAC0_TRIG @0x300. */
#define INPUTMUX0    0x40006000u
#define DAC0_TRIG    (*(volatile uint32_t *)(INPUTMUX0 + 0x300))
#define TRIG_SRC_CTIMER0_M3 5u       /* decoded from NXP's compiled driver */

#define NSAMP 4

static uint32_t fpr_rptr(void)
{
    return DAC_FPR & 0xFFFFu;
}

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    int i;
    uint32_t rptr1, fsr2;

    LP_CTRL = CTRL_TE;
    puts_("DAC-HWTRIG test\r\n");

    /* CTIMER0 on FRO_HF. */
    SCG_FIRCCSR   = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = SEL_FROHF;

    /* DAC0 enabled, FIFO on; fill 4 samples.  Start in SOFTWARE-trigger mode. */
    DAC_GCR = GCR_DACEN | GCR_FIFOEN | GCR_TRGSEL;
    for (i = 0; i < NSAMP; i++) {
        DAC_DATA = 0x100u + i;               /* 4 distinct samples */
    }
    DAC0_TRIG = TRIG_SRC_CTIMER0_M3;

    /* CTIMER0 match-3, periodic. */
    CT0_MR3 = MR3_COUNTS;
    CT0_MCR = MCR_RST3;
    CT0_TCR = TCR_RST;
    CT0_TCR = TCR_RUN;

    /*
     * PHASE 1 -- SOFTWARE-trigger mode: the routed hardware trigger must be IGNORED.
     * The FIFO read pointer must not move.
     */
    for (d = 0; d < 400000; d++) {
    }
    rptr1 = fpr_rptr();
    puts_("  phase1 (software mode) rptr = "); puthex(rptr1); puts_("\r\n");
    if (rptr1 != 0) {
        puts_("  FAIL: hardware trigger advanced the FIFO in SOFTWARE mode\r\n");
        ok = 0;
    }
    if (DAC_FSR & FSR_EMPTY) {
        puts_("  FAIL: FIFO drained in software mode\r\n");
        ok = 0;
    }

    /*
     * PHASE 2 -- HARDWARE-trigger mode: each match pops a sample.  4 samples drain to
     * EMPTY, and the continuing matches UNDERFLOW an empty FIFO (FSR[UF]).
     */
    DAC_GCR = GCR_DACEN | GCR_FIFOEN;        /* TRGSEL=0 -> hardware trigger */
    for (d = 0; d < 400000; d++) {
    }
    CT0_TCR = 0;                             /* stop */
    fsr2 = DAC_FSR;
    puts_("  phase2 (hardware mode) FSR = "); puthex(fsr2);
    puts_("  rptr = "); puthex(fpr_rptr()); puts_("\r\n");

    ok &= !!(fsr2 & FSR_EMPTY);              /* the FIFO drained */
    ok &= !!(fsr2 & FSR_UF);                 /* and over-triggered -> underflow */

    puts_(ok ? "DAC-HWTRIG PASS\r\n" : "DAC-HWTRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
