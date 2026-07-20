/*
 * MCXN947 MRT (Multi-Rate Timer) — THE BLOCK THAT HAD NO TEST.
 *
 * ⭐ AND IT IS THE ABSENCE OF THIS TEST THAT MADE A FALLBACK UNFINDABLE.
 *
 * mrt_freq() used to read:
 *
 *     uint32_t hz = s->clk ? clock_get_hz(s->clk) : 0;
 *     return hz ? hz : 150000000;
 *
 * The MRT's clock IS wired (to sysclk), and sysclk IS 150 MHz -- so the fallback
 * HAPPENED TO EQUAL THE ANSWER.
 *
 *   ⭐ "A DEFAULT THAT HAPPENS TO EQUAL THE ANSWER IS NOT AN ANSWER.  IT IS A GREEN
 *      LIGHT WITH NO WITNESS BEHIND IT."                              (93emulator)
 *
 * Unwire the clock and it would have gone on returning 150 MHz, and NOTHING could have
 * told -- the derived value and the invented value are the same number.  Invisible twice
 * over: the default was right, AND NOBODY WAS LOOKING.
 *
 * So this test does NOT assert a ratio.  A ratio between two MRT intervals cancels the
 * clock rate out -- and the rate is the whole question.  It measures the MRT's interval
 * against SysTick, which runs at the CPU clock.  Both are fed from the same 150 MHz
 * sysclk, so:
 *
 *     SysTick ticks for an MRT interval of N counts  ==  N   (exactly)
 *
 *   ⭐ AN ABSOLUTE MEASUREMENT AGAINST AN INDEPENDENT CLOCK IS THE ONLY THING THAT CAN
 *      SEE A RATE THAT WAS INVENTED RATHER THAN DERIVED.  Poison the fallback and this
 *      test moves; poison it under a RATIO test and nothing moves at all.
 */
#include <stdint.h>

#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define MRT0      0x40013000u          /* MRT0 (non-secure alias) */
#define CH(n)     (MRT0 + (n) * 0x10u)
#define CH_INTVAL 0x0
#define CH_TIMER  0x4
#define CH_CTRL   0x8
#define CH_STAT   0xC

#define INTVAL_LOAD   (1u << 31)
#define CTRL_INTEN    (1u << 0)
#define CTRL_ONESHOT  (1u << 1)        /* MODE = 01: one-shot */
#define STAT_INTFLAG  (1u << 0)
#define STAT_RUN      (1u << 1)

#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

#define LPUART4   0x400B4000u
#define LP_STAT   (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL   (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA   (*(volatile uint32_t *)(LPUART4 + 0x1C))
#define STAT_TDRE (1u << 23)
#define CTRL_TE   (1u << 19)

static void putc_(char c) { while (!(LP_STAT & STAT_TDRE)) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) { putc_(*s++); } }
static void putdec(uint32_t v)
{
    char b[12]; int i = 0;
    if (!v) { putc_('0'); return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i) { putc_(b[--i]); }
}

/* Run one MRT one-shot of `counts` and return the SysTick ticks it took. */
static uint32_t measure(uint32_t counts)
{
    uint32_t t0, t1;

    MEM32(CH(0) + CH_CTRL) = 0;                  /* stop */
    MEM32(CH(0) + CH_STAT) = STAT_INTFLAG;       /* W1C any stale flag */

    SYST_RVR = SYST_MASK;
    SYST_CVR = 0;
    SYST_CSR = (1u << 0) | (1u << 2);            /* enable, processor clock */

    MEM32(CH(0) + CH_CTRL)   = CTRL_ONESHOT;
    t0 = SYST_CVR;
    MEM32(CH(0) + CH_INTVAL) = INTVAL_LOAD | counts;   /* LOAD: start now */

    while (!(MEM32(CH(0) + CH_STAT) & STAT_INTFLAG)) {
    }
    t1 = SYST_CVR;
    MEM32(CH(0) + CH_STAT) = STAT_INTFLAG;
    MEM32(CH(0) + CH_CTRL) = 0;

    return (t0 - t1) & SYST_MASK;                /* SysTick counts DOWN */
}

void cpu0_main(void)
{
    uint32_t a, b;
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("MRT test\r\n");

    /*
     * ⭐ THE SHARED-CLOCK CHECK.  MRT runs on the AHB/bus clock = the SCG main clock, the
     *   SAME clock SysTick derives from -- so an MRT interval of N counts must take N SysTick
     *   ticks, WHATEVER that clock is.  This test runs at the reset core clock (FRO_HF,
     *   48 MHz -- it does NOT configure the PLL), and 200000 counts still == 200000 ticks
     *   because both timers move together.  A model that pinned MRT to a 150 MHz constant
     *   (the old `?: 150000000` / raw sysclk wire) while SysTick sits at the 48 MHz reset
     *   clock reads ~64000 ticks for 200000 counts -- caught here.  The rate cancels in a
     *   ratio test; this ABSOLUTE 1:1 is what proves MRT and SysTick share the derived clock.
     */
    a = measure(200000u);
    puts_("  MRT 200000 counts -> "); putdec(a); puts_(" SysTick ticks\r\n");

    b = measure(400000u);
    puts_("  MRT 400000 counts -> "); putdec(b); puts_(" SysTick ticks\r\n");

    /* Absolute: within 2% (the polling loop's own overhead is the only slack). */
    ok &= (a > 196000u && a < 204000u);
    ok &= (b > 392000u && b < 408000u);

    /* And it must SCALE -- so a model that pins a constant period fails too. */
    ok &= (b > a * 19u / 10u && b < a * 21u / 10u);

    puts_(ok ? "MRT PASS\r\n" : "MRT FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[64] = {
    [0] = (vec_t)0x20010000u,
    [1] = cpu0_main,
};
