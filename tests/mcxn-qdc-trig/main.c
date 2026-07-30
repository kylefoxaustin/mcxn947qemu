/*
 * QDC POSITION CAPTURE ON A TIMER -- the quadrature decoder's hardware-trigger path.
 *
 *     CTIMER0 match-3  ->  INPUTMUX (selector 5 -> QDC0_TRIG)
 *                      ->  QDC0 position capture/clear (CTRL2[UPDHLD]/[UPDPOS]).
 *
 * A field-oriented-control loop snapshots the encoder position synchronised to the PWM
 * carrier, so it reads a COHERENT {upper, lower, revolution} sample taken at one instant
 * (via the hold registers) instead of across three racing reads.  It was inert: QDC0_TRIG
 * was a stored INPUTMUX selector reaching nothing, so neither UPDHLD (capture) nor UPDPOS
 * (clear) could ever happen on a trigger.
 *
 * The guest initialises the position counters (UPOS/LPOS/REV) -- as an encoder would --
 * and a timer then captures or clears them:
 *   PHASE A (CTRL2[UPDHLD]): the trigger snapshots UPOS/LPOS/REV -> UPOSH/LPOSH/REVH, and
 *           MUST NOT clear the live counters.
 *   PHASE B (CTRL2[UPDPOS]): the trigger CLEARS UPOS/LPOS/REV to zero (a timed re-zero).
 *
 * Selector 5 = CTIMER0 match-3, DERIVED (kINPUTMUX_Ctimer0M3ToQdc0Trigger = 5).  The CPU
 * never writes CTRL[SWIP].  (The position ADVANCEMENT -- the PHASEA/PHASEB quadrature decode
 * -- has no signal source in emulation and is a stated seam; the trigger's capture/clear of
 * whatever position is present is the whole function of QDCn_TRIG, and that is what runs.)
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

/* ---- QDC0 (base 0x400CF000) ---------------------------------------------- */
#define QDC0 0x400CF000u
#define QDC_REV   (*(volatile uint16_t *)(QDC0 + 0x0A))
#define QDC_REVH  (*(volatile uint16_t *)(QDC0 + 0x0C))
#define QDC_UPOS  (*(volatile uint16_t *)(QDC0 + 0x0E))
#define QDC_LPOS  (*(volatile uint16_t *)(QDC0 + 0x10))
#define QDC_UPOSH (*(volatile uint16_t *)(QDC0 + 0x12))
#define QDC_LPOSH (*(volatile uint16_t *)(QDC0 + 0x14))
#define QDC_CTRL2 (*(volatile uint16_t *)(QDC0 + 0x1E))
#define CTRL2_UPDHLD (1u << 0)
#define CTRL2_UPDPOS (1u << 1)

/* INPUTMUX0: QDC0_TRIG @0x360. */
#define INPUTMUX0    0x40006000u
#define QDC0_TRIG    (*(volatile uint32_t *)(INPUTMUX0 + 0x360))
#define TRIG_SRC_CTIMER0_M3 5u

#define POS_U  0x1234u
#define POS_L  0x5678u
#define POS_R  0x000Au

static int wait_until(volatile uint16_t *reg, uint16_t want)
{
    volatile int d;

    for (d = 0; d < 40000000; d++) {
        if (*reg == want) {
            return 1;
        }
    }
    return *reg == want;
}

void cpu0_main(void)
{
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("QDC-TRIG test\r\n");

    SCG_FIRCCSR   = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = SEL_FROHF;

    /* Initialise the encoder position (as a real encoder / init would). */
    QDC_UPOS = POS_U;
    QDC_LPOS = POS_L;
    QDC_REV  = POS_R;

    QDC0_TRIG = TRIG_SRC_CTIMER0_M3;
    CT0_MR3 = MR3_COUNTS;
    CT0_MCR = MCR_RST3;
    CT0_TCR = TCR_RST;
    CT0_TCR = TCR_RUN;

    /* PHASE A -- UPDHLD: a trigger snapshots the position into the hold registers. */
    QDC_CTRL2 = CTRL2_UPDHLD;
    wait_until(&QDC_UPOSH, POS_U);
    puts_("  UPOSH="); puthex(QDC_UPOSH);
    puts_(" LPOSH="); puthex(QDC_LPOSH);
    puts_(" REVH="); puthex(QDC_REVH); puts_("\r\n");
    ok &= (QDC_UPOSH == POS_U);           /* snapshot captured */
    ok &= (QDC_LPOSH == POS_L);
    ok &= (QDC_REVH  == POS_R);
    ok &= (QDC_UPOS  == POS_U);           /* UPDHLD must NOT clear the live counter */
    ok &= (QDC_LPOS  == POS_L);
    puts_(ok ? "  phaseA: captured, position intact (correct)\r\n"
             : "  phaseA: FAILED\r\n");

    /* PHASE B -- UPDPOS: a trigger clears the live position counters. */
    QDC_CTRL2 = CTRL2_UPDPOS;
    wait_until(&QDC_UPOS, 0);
    puts_("  after clear: UPOS="); puthex(QDC_UPOS);
    puts_(" LPOS="); puthex(QDC_LPOS);
    puts_(" REV="); puthex(QDC_REV); puts_("\r\n");
    ok &= (QDC_UPOS == 0);
    ok &= (QDC_LPOS == 0);
    ok &= (QDC_REV  == 0);
    CT0_TCR = 0;

    puts_(ok ? "QDC-TRIG PASS\r\n" : "QDC-TRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
