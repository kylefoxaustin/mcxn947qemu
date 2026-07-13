/*
 * MCXN947 SCT: the counter PERIOD, measured — and the PRESCALER, swept.
 *
 * ⭐ WHAT THIS TEST USED TO BE, AND WHY THAT WAS DECORATION.
 *
 * It counted interrupts:  "events >= 3 ? PASS : FAIL".  That is rung ONE of the
 * ladder -- "did it tick?" -- and it is blind to the two things that were actually
 * wrong:
 *
 *   1. THE TICK RATE WAS INVENTED.  The model ran at a hardcoded 10 ns/tick,
 *      commented "nominal ~100 MHz" -- a number that appears NOWHERE in the RM.  It
 *      is the IDENTICAL constant already removed from the eFlexPWM, and I fixed it
 *      THERE and left it sitting HERE.  A FIX APPLIED IN ONE PLACE IS NOT A FIX.
 *      (And the word was the tell: "nominal", "plausible", "approximate",
 *      "best-effort" ARE THE WORDS YOU USE WHEN YOU MEAN FABRICATED.)
 *
 *   2. THE PRESCALER WAS NOT MODELLED AT ALL.  CTRL[PRE_L] (bits 12:5) divides the
 *      counter clock by PRE_L+1, and the model IGNORED IT -- so a driver asking for
 *      a 256x slower count got EXACTLY THE SAME RATE, silently.  A test that counts
 *      interrupts can never see that, because the interrupts still arrive.
 *
 *      ⭐ ONE SHAPE IS A COLLAPSED ORACLE.  A test that never sweeps an axis cannot
 *         tell you the axis is not wired up.  Sweep every axis you claim.
 *
 * SO: PREDICT the period from configuration, MEASURE it against an INDEPENDENT time
 * base (SysTick, which counts processor clocks and knows nothing about the SCT), and
 * SWEEP THE PRESCALER so that a prescaler which does nothing CANNOT pass.
 *
 * THE GOLDEN IS THE RM'S SEMANTICS, NOT THE MODEL'S CONSTANT.  The SCT counter is
 * clocked from the SCT clock divided by CTRL[PRE_L]+1, and SysTick is clocked from
 * the processor clock; both derive from the SoC system clock, so:
 *
 *     SysTick ticks per SCT period  ==  (MATCHREL0 + 1) * (PRE_L + 1)     (exactly)
 *
 * That RATIO is exact and is what this asserts.  The ABSOLUTE frequency remains a
 * documented assumption (the clock tree is not modelled) -- which is precisely why
 * the test must not depend on it, and does not.
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

#define SCT0 0x40091000u
#define SCT_CTRL      (*(volatile uint32_t *)(SCT0 + 0x004))
#define SCT_EVEN      (*(volatile uint32_t *)(SCT0 + 0x0F0))
#define SCT_EVFLAG    (*(volatile uint32_t *)(SCT0 + 0x0F4))
#define SCT_MATCHREL0 (*(volatile uint32_t *)(SCT0 + 0x180))

#define CTRL_CLRCTR_L (1u << 3)
#define CTRL_HALT_L   (1u << 2)
#define EV0           (1u << 0)

/* CTRL[PRE_L]: CMSIS SCT_CTRL_PRE_L_MASK = 0x1FE0, shift 5.  Divides by PRE_L + 1. */
#define CTRL_PRE_L(n) ((uint32_t)((n) & 0xFFu) << 5)

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define SCT0_IRQ 33

/* SysTick — an INDEPENDENT time base.  It measures elapsed processor clocks and
 * knows nothing whatever about the SCT, so it cannot be fooled by an SCT that
 * agrees with itself. */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define SYST_ENABLE    (1u << 0)
#define SYST_CLKSOURCE (1u << 2)   /* processor clock */
#define SYST_MASK  0x00FFFFFFu     /* 24-bit down-counter */

#define MATCHREL   0x200
#define PERIODS    4
/* From the RM's semantics, NOT from the model. */
#define EXPECT_FOR(pre) ((uint32_t)(MATCHREL + 1) * ((pre) + 1) * PERIODS)

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

static void putdec(uint32_t v)
{
    char b[12];
    int i = 0;

    if (!v) {
        putc_('0');
        return;
    }
    while (v) {
        b[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) {
        putc_(b[i]);
    }
}

static volatile uint32_t events;
static volatile uint32_t t_first, t_last;

void sct0_handler(void)
{
    SCT_EVFLAG = EV0;                 /* W1C */
    events++;
    if (events == 1) {
        t_first = SYST_CVR;
    } else if (events == 1 + PERIODS) {
        t_last = SYST_CVR;
    }
}

void cpu0_main(void)
{
    int ok = 1;
    int p;

    LP_CTRL = CTRL_TE;
    puts_("SCT test\r\n");

    SYST_RVR = SYST_MASK;
    SYST_CVR = 0;
    SYST_CSR = SYST_ENABLE | SYST_CLKSOURCE;

    NVIC_ISER1 = (1u << (SCT0_IRQ - 32));

    /*
     * SWEEP THE PRESCALER.  If the model ignores CTRL[PRE_L] -- which it did -- the
     * PRE_L=1 and PRE_L=7 rows come back at the PRE_L=0 period and FAIL right here.
     * A single shape could not have told me anything.
     */
    for (p = 0; p < 3; p++) {
        static const uint8_t pre_list[3] = { 0, 1, 7 };   /* /1, /2, /8 */
        uint8_t pre = pre_list[p];
        uint32_t expect = EXPECT_FOR(pre);
        uint32_t elapsed, diff;

        events = 0;
        SCT_CTRL = CTRL_HALT_L | CTRL_CLRCTR_L | CTRL_PRE_L(pre);
        SCT_MATCHREL0 = MATCHREL;
        SCT_EVFLAG = EV0;
        SCT_EVEN = EV0;

        __asm__ volatile ("cpsie i");
        SCT_CTRL = CTRL_PRE_L(pre);       /* clear HALT -> run */

        while (events < 1 + PERIODS) {
        }

        SCT_CTRL = CTRL_HALT_L | CTRL_PRE_L(pre);
        __asm__ volatile ("cpsid i");

        elapsed = (t_first - t_last) & SYST_MASK;   /* SysTick counts DOWN */
        diff = (elapsed > expect) ? (elapsed - expect) : (expect - elapsed);

        puts_("  PRE_L="); putdec(pre);
        puts_(" measured "); putdec(elapsed);
        puts_(" SysTick ticks, expected "); putdec(expect);
        puts_("\r\n");

        /* Within 1%: the period must scale EXACTLY with PRE_L + 1. */
        ok &= (diff * 100 <= expect);
    }

    puts_(ok ? "SCT PASS\r\n" : "SCT FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[60] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + SCT0_IRQ] = sct0_handler,  /* exception 49 = IRQ 33 */
};
