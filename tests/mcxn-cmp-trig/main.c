/*
 * CMP ROUND-ROBIN ON A TIMER -- the comparator's hardware-trigger path.
 *
 *     CTIMER0 match-3  ->  INPUTMUX (selector 5 -> CMP0_TRIG)
 *                      ->  CMP0 round-robin sample (RRCR0[RR_EN], external trigger)
 *                      ->  CSR[RRF] latches when the monitored input DEVIATES from the
 *                          state captured when round-robin was enabled (RR_INITMOD).
 *
 * This is the low-power "watch a signal, wake on change" path: a timer paces the
 * comparator's round-robin sampling with no CPU involvement, and RRF flags a deviation.
 * It was inert -- CMP0_TRIG was a stored INPUTMUX selector reaching nothing, and RRF
 * could never set.
 *
 * The comparator input is analog, so it is OPERATOR-DRIVEN over QMP ("comparator-output"),
 * exactly like mcxn-cmp-dma.  Three phases:
 *   A (baseline):        output == the captured baseline -> triggered samples do NOT flag.
 *   B (deviation):       the operator drives the output high -> the next triggered sample
 *                        deviates from the baseline -> RRF sets.
 *   C (internal mode):   RRCR0[RR_TRG_SEL]=1 selects the internal timer -> the routed
 *                        EXTERNAL trigger must be IGNORED -> RRF stays clear despite the
 *                        still-deviating input.
 *
 * Selector 5 = CTIMER0 match-3, DERIVED (kINPUTMUX_Ctimer0M3ToCmp0Trigger = 5).
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

/* ---- clocks (CTIMER0 <- FRO_HF) ------------------------------------------ */
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
#define MCR_RST3 (1u << 10)
#define MR3_COUNTS 100u

/* ---- CMP0 (base 0x40051000) ---------------------------------------------- */
#define CMP0 0x40051000u
#define CMP_CSR   (*(volatile uint32_t *)(CMP0 + 0x20))
#define CMP_RRCR0 (*(volatile uint32_t *)(CMP0 + 0x24))
#define CSR_RRF        (1u << 2)     /* round-robin flag, W1C */
#define RRCR0_RR_EN      (1u << 0)
#define RRCR0_RR_TRG_SEL (1u << 1)   /* 0 = external trigger, 1 = internal timer */

/* INPUTMUX0: CMP0_TRIG @0x260. */
#define INPUTMUX0    0x40006000u
#define CMP0_TRIG    (*(volatile uint32_t *)(INPUTMUX0 + 0x260))
#define TRIG_SRC_CTIMER0_M3 5u

static void spin(int n)
{
    volatile int d;

    for (d = 0; d < n; d++) {
    }
}

void cpu0_main(void)
{
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("CMP-TRIG test\r\n");

    /* CTIMER0 on FRO_HF. */
    SCG_FIRCCSR   = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = SEL_FROHF;

    /* Enable round-robin in EXTERNAL-trigger mode; the operator output is low, so the
     * captured baseline is low.  Route CTIMER0 M3 -> CMP0_TRIG and start the timer. */
    CMP_RRCR0 = RRCR0_RR_EN;             /* baseline := comparator-output (low) */
    CMP0_TRIG = TRIG_SRC_CTIMER0_M3;
    CT0_MR3 = MR3_COUNTS;
    CT0_MCR = MCR_RST3;
    CT0_TCR = TCR_RST;
    CT0_TCR = TCR_RUN;

    /* PHASE A -- output still at the baseline: triggered samples must NOT flag. */
    spin(2000000);
    if (CMP_CSR & CSR_RRF) {
        puts_("  FAIL: RRF set with the input at its baseline (no deviation)\r\n");
        ok = 0;
    } else {
        puts_("  phaseA: baseline, RRF clear (correct)\r\n");
    }

    /* Hand off to the operator: drive the comparator output HIGH (a deviation).  Then
     * WAIT for a triggered sample to flag it -- polling, not a fixed delay, so the guest
     * cannot race past the operator's QMP injection (the fixed-delay version did). */
    puts_("CMP-TRIG ARMED\r\n");
    {
        volatile int d;

        for (d = 0; d < 80000000 && !(CMP_CSR & CSR_RRF); d++) {
        }
    }

    /* PHASE B -- the input now deviates from the baseline: a triggered sample sets RRF. */
    if (CMP_CSR & CSR_RRF) {
        puts_("  phaseB: deviation -> RRF set (correct)\r\n");
    } else {
        puts_("  FAIL: RRF did not set after the input deviated\r\n");
        ok = 0;
    }

    /* PHASE C -- switch to INTERNAL-trigger mode: the routed external trigger must be
     * ignored, so a fresh RRF must NOT appear even though the input still deviates.
     * Select internal mode BEFORE clearing RRF, or a trigger firing in between (still
     * external mode) re-sets the flag we just cleared. */
    CMP_RRCR0 = RRCR0_RR_EN | RRCR0_RR_TRG_SEL;   /* internal timer (no re-baseline) */
    CMP_CSR   = CSR_RRF;                  /* W1C clear the phase-B flag */
    spin(4000000);
    if (CMP_CSR & CSR_RRF) {
        puts_("  FAIL: external trigger sampled in INTERNAL-trigger mode\r\n");
        ok = 0;
    } else {
        puts_("  phaseC: internal mode, external trigger ignored (correct)\r\n");
    }
    CT0_TCR = 0;

    puts_(ok ? "CMP-TRIG PASS\r\n" : "CMP-TRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
