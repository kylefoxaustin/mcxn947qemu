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
 * ⭐ AND THE COMMENT THAT USED TO SIT HERE WAS FALSE, IN THE MOST INSTRUCTIVE WAY.
 *
 * It said: "SysTick ticks per SCT period == (MATCHREL0+1) * (PRE_L+1), exactly.  That
 * RATIO is exact... the absolute frequency remains a documented assumption -- WHICH IS
 * PRECISELY WHY THE TEST MUST NOT DEPEND ON IT, AND DOES NOT."
 *
 * IT DID.  That prediction is only true if THE SCT CLOCK EQUALS THE CPU CLOCK -- which
 * was the MODEL'S constant (SCT_CLOCK_HZ = 150 MHz = sysclk), not silicon's.  Every
 * stock example does CLOCK_AttachClk(kFRO_HF_to_SCT): the real SCT runs at FRO_HF,
 * 48 MHz.  THE MODEL WAS 3.1x TOO FAST AND THE TEST PREDICTED THE SAME WRONG NUMBER,
 * SO IT PASSED.
 *
 *     ⭐ A MIRROR THAT DECLARES ITSELF IS STILL A MIRROR -- and this one went further:
 *        it declared itself and then DENIED it in the same paragraph.
 *
 * (rt1180emulator shipped the identical specimen and named it: a PWM golden whose own
 * comment read "if PWM_CLK were wrong, this golden would be wrong in exactly the same
 * direction AND STILL PASS."  It was.  It did.)
 *
 * SO THE TEST NOW PROGRAMS THE CLOCK, LIKE FIRMWARE DOES, AND SWEEPS IT AS AN AXIS.
 * Three axes, and every golden is a RATIO taken from the SDK's own source rates, so
 * NONE of them depends on our assumed system clock:
 *
 *     CTRL[PRE_L]      /1 -> /2 -> /8    (the counter prescaler)
 *     SCTCLKDIV        /1 -> /4          (the clock-tree divider)
 *     FIRCCFG[RANGE]   48 MHz -> 144 MHz (the SOURCE itself: exactly 3x)
 *
 * A model that ignores the PRESCALER fails axis 1.  One that ignores the clock-tree
 * DIVIDER fails axis 2.  One that ignores THE CLOCK TREE ENTIRELY -- which is what this
 * model did -- reports THE SAME PERIOD at 48 MHz and at 144 MHz, and fails axis 3.
 * The old test could not fail any of them.
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

/* SCG0 — the SOURCE clocks.  FRO_HF is 48 MHz (FIRCCFG[RANGE]=0) or 144 (RANGE=1). */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_FIRCCFG (*(volatile uint32_t *)(SCG0 + 0x308))
#define FIRCCSR_FIRCEN (1u << 0)
#define FIRCCFG_RANGE  (1u << 0)

/* SYSCON — CMSIS: SCTCLKSEL @0x2F0, SCTCLKDIV @0x3B4. */
#define SCTCLKSEL (*(volatile uint32_t *)0x400002F0u)
#define SCTCLKDIV (*(volatile uint32_t *)0x400003B4u)
#define SEL_FROHF 3u        /* CLOCK_AttachClk(kFRO_HF_to_SCT) -- what firmware does */

#define MATCHREL   0x200
#define PERIODS    4

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

/* Time PERIODS SCT periods against SysTick, which knows nothing about the SCT. */
static uint32_t measure(uint8_t pre)
{
    events = 0;
    SCT_CTRL = CTRL_HALT_L | CTRL_CLRCTR_L | CTRL_PRE_L(pre);
    SCT_MATCHREL0 = MATCHREL;
    SCT_EVFLAG = EV0;
    SCT_EVEN = EV0;

    SCT_CTRL = CTRL_PRE_L(pre);       /* clear HALT -> run */
    while (events < 1 + PERIODS) {
    }
    SCT_CTRL = CTRL_HALT_L | CTRL_PRE_L(pre);

    return (t_first - t_last) & SYST_MASK;   /* SysTick counts DOWN */
}

static int near(uint32_t got, uint32_t want, uint32_t tol_pct)
{
    uint32_t d = got > want ? got - want : want - got;

    return (uint64_t)d * 100u <= (uint64_t)want * tol_pct;
}

/* Bring the core/SysTick clock to 150 MHz via PLL0, as BOARD_InitBootClocks does
 * (the SCG boots on FRO_HF at 48 MHz; timing measured vs SysTick needs the 150 MHz
 * operating point this test's goldens assume). */
static void clock_init_150m(void)
{
    volatile uint32_t *scg = (volatile uint32_t *)0x40044000u;
    scg[0x300 / 4] |= 1u;             /* FIRCCSR |= FIRCEN           */
    scg[0x504 / 4]  = 0x020035B0u;    /* APLLCTRL: SOURCE=1 (Clk48M) */
    scg[0x50C / 4]  = 8u;             /* APLLNDIV N=8                */
    scg[0x510 / 4]  = 50u;            /* APLLMDIV M=50               */
    scg[0x514 / 4]  = 1u;             /* APLLPDIV P=1                */
    scg[0x500 / 4] |= 3u;             /* APLLCSR PWREN|CLKEN         */
    scg[0x014 / 4]  = (5u << 24);     /* RCCR SCS = PLL0 -> 150 MHz  */
}

void cpu0_main(void)
{
    uint32_t t_base, t_pre2, t_pre8, t_div4, t_144m;
    int ok = 1;

    LP_CTRL = CTRL_TE;
    clock_init_150m();
    puts_("SCT test\r\n");

    SYST_RVR = SYST_MASK;
    SYST_CVR = 0;
    SYST_CSR = SYST_ENABLE | SYST_CLKSOURCE;

    NVIC_ISER1 = (1u << (SCT0_IRQ - 32));
    __asm__ volatile ("cpsie i");

    /* Program the clock EXACTLY as BOARD_InitBootClocks + the stock example do:
     * enable the FIRC, pick the 48 MHz range, and attach FRO_HF to the SCT. */
    SCG_FIRCCFG = 0;                          /* RANGE = 0 -> FRO_HF = 48 MHz */
    SCG_FIRCCSR = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    SCTCLKDIV   = 0;                          /* divide by 1 */
    SCTCLKSEL   = SEL_FROHF;                  /* CLOCK_AttachClk(kFRO_HF_to_SCT) */

    /* ---- axis 1: the counter prescaler, CTRL[PRE_L] ---------------------- */
    t_base = measure(0);
    t_pre2 = measure(1);
    t_pre8 = measure(7);
    puts_("  PRE_L=0 (FRO_HF 48MHz, /1) : "); putdec(t_base); puts_(" SysTick ticks\r\n");
    puts_("  PRE_L=1  -> must be 2.00x  : "); putdec(t_pre2); puts_("\r\n");
    puts_("  PRE_L=7  -> must be 8.00x  : "); putdec(t_pre8); puts_("\r\n");
    ok &= near(t_pre2, t_base * 2u, 2);
    ok &= near(t_pre8, t_base * 8u, 2);

    /* ---- axis 2: the CLOCK-TREE divider, SYSCON[SCTCLKDIV] ---------------- */
    SCTCLKDIV = 3;                            /* DIV field: divide by DIV+1 = 4 */
    t_div4 = measure(0);
    puts_("  SCTCLKDIV=/4 -> must be 4.00x: "); putdec(t_div4); puts_("\r\n");
    ok &= near(t_div4, t_base * 4u, 2);
    SCTCLKDIV = 0;

    /* ---- axis 3: THE SOURCE ITSELF.  FRO_HF 48 -> 144 MHz is exactly 3x, and a
     * model that ignores the clock tree reports THE SAME PERIOD for both. --- */
    SCG_FIRCCFG = FIRCCFG_RANGE;              /* FRO_HF = 144 MHz */
    t_144m = measure(0);
    puts_("  FRO_HF 144MHz -> must be /3 : "); putdec(t_144m);
    puts_("  (48/144 = 3.00x faster)\r\n");
    ok &= near(t_144m * 3u, t_base, 3);

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
