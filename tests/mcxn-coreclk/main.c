/*
 * MCXN947 the M33 CORE CLOCK (and SysTick) is DERIVED from the SCG, not a fixed constant.
 *
 * Out of reset the SCG main clock is RCCR[SCS]=3 = FRO_HF = 48 MHz, so the core boots at
 * 48 MHz -- exactly as silicon does before BOARD_InitBootClocks.  Bringing up PLL0 and
 * switching the main clock to it raises the core (and SysTick) to 150 MHz.
 *
 * Proof, against a reference that does NOT move with the core clock: a CTIMER clocked from
 * FRO12M (12 MHz SIRC, generated independently of the PLL/main clock) ticks at a fixed rate,
 * so one CTIMER interval is a fixed wall-clock time.  Measure it with SysTick BEFORE the PLL
 * is configured (SysTick @ 48 MHz reset core) and AFTER (SysTick @ 150 MHz): the SAME interval
 * now costs 150/48 = 3.125x more SysTick ticks, because SysTick's clock moved.
 *
 * (The MRT can't be the reference any more -- it runs on the bus clock = main clock, so it
 * now moves WITH SysTick; both would scale together and the derivation would be invisible.
 * That is exactly why the reference must be on a fixed source like FRO12M.)
 *
 *   A model that pinned the core clock to a 150 MHz constant reads the SAME count both times.
 *
 * ⚠ -icount shift=3 REQUIRED: the measurement is in virtual SysTick ticks.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

/* ---- CTIMER0 on FRO12M (12 MHz, fixed real-time reference) ---------------- */
#define CTIMER0CLKSEL (*(volatile uint32_t *)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t *)0x400003D0u)
#define CTSEL_FRO12M  4u
#define CT0 0x4000C000u
#define CT_IR  (*(volatile uint32_t *)(CT0 + 0x00))
#define CT_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT_PR  (*(volatile uint32_t *)(CT0 + 0x0C))
#define CT_MCR (*(volatile uint32_t *)(CT0 + 0x14))
#define CT_MR0 (*(volatile uint32_t *)(CT0 + 0x18))

/* ---- SysTick (on the core clock = the SCG main clock) --------------------- */
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

#define LPUART4   0x400B4000u
#define LP_STAT   (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL   (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA   (*(volatile uint32_t *)(LPUART4 + 0x1C))
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putdec(uint32_t v) { char b[12]; int i = 0; if (!v) { putc_('0'); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; } while (i--) putc_(b[i]); }

#define MR0_COUNTS 1200u

/* Time MR0_COUNTS CTIMER (FRO12M 12 MHz) ticks against SysTick (which counts DOWN). */
static uint32_t measure(void)
{
    uint32_t t0, t1;

    CT_TCR = 2; CT_PR = 0; CT_MR0 = MR0_COUNTS; CT_MCR = 1u; CT_IR = 0xFF;
    SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = (1u << 0) | (1u << 2);
    t0 = SYST_CVR;
    CT_TCR = 1;
    while (!(CT_IR & 1u)) {
    }
    t1 = SYST_CVR;
    CT_TCR = 2;
    return (t0 - t1) & SYST_MASK;
}

/* Bring the core clock to 150 MHz via PLL0, as BOARD_InitBootClocks does. */
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
    uint32_t a, b;
    int ok = 1;

    LP_CTRL = (1u << 19);
    puts_("CORECLK test\r\n");

    /* CTIMER0 on FRO12M (12 MHz): a fixed real-time reference, independent of the core. */
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = CTSEL_FRO12M;

    /* (1) Reset core clock = FRO_HF = 48 MHz.  1200 CTIMER ticks @12 MHz = 100 us, measured
     *     by a 48 MHz SysTick = 100us*48MHz = 4800 ticks. */
    a = measure();
    puts_("  reset core (FRO_HF 48MHz): 1200 CTIMER ticks -> "); putdec(a); puts_(" SysTick\r\n");

    /* (2) Raise the core to 150 MHz via PLL0. */
    clock_init_150m();

    /* (3) Same CTIMER interval, now measured by a 150 MHz SysTick = 100us*150MHz = 15000. */
    b = measure();
    puts_("  after PLL0 (150MHz):       1200 CTIMER ticks -> "); putdec(b); puts_(" SysTick\r\n");

    ok &= (a > 4704u && a < 4896u);              /* 48 MHz:  ~4800  (1200/12M * 48M)  */
    ok &= (b > 14700u && b < 15300u);            /* 150 MHz: ~15000 (1200/12M * 150M) */
    /* The SysTick clock jumped 150/48 = 3.125x: the core clock is DERIVED, not pinned. */
    ok &= (b > a * 30u / 10u && b < a * 33u / 10u);

    puts_(ok ? "CORECLK PASS\r\n" : "CORECLK FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[64] = { [0] = (vec_t)0x20010000u, [1] = cpu0_main };
