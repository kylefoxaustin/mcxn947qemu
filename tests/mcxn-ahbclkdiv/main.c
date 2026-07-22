/*
 * MCXN947 the AHB/bus clock is main clock / (AHBCLKDIV+1) — the core + SysTick follow it.
 *
 * The M33 core, SysTick, MRT and FlexPWM run on the AHB bus clock, which SYSCON derives as
 * mainclk / (SYSCON.AHBCLKDIV + 1).  AHBCLKDIV resets to 0 (divide-by-1), so out of reset the
 * core sees the full main clock; a non-zero AHBCLKDIV divides it down.  The old model routed
 * the core straight off the main clock, ignoring AHBCLKDIV entirely.
 *
 * Proof, against an INDEPENDENT reference (a CTIMER clocked from FRO_12M = 12 MHz, which does
 * NOT go through AHBCLKDIV): time a fixed CTIMER interval with SysTick at AHBCLKDIV=0 (core =
 * 150 MHz) and again at AHBCLKDIV=1 (core = 75 MHz).  SysTick counts at its own clock, so over
 * the SAME real-time interval a HALVED SysTick clock counts HALF the ticks: div2 = div1/2,
 * ratio 0.5.  A model that ignored AHBCLKDIV keeps SysTick at 150 MHz -> div2 = div1 -> ratio 1.
 *
 * ⚠ -icount shift=3 REQUIRED: the measurement is in virtual SysTick ticks.
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
static void putdec(uint32_t v) { char b[12]; int i = 0; if (!v) { putc_('0'); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; } while (i--) putc_(b[i]); }

/* ---- CTIMER0 on FRO_12M (12 MHz, independent of the AHB clock) ------------ */
#define CTIMER0CLKSEL (*(volatile uint32_t *)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t *)0x400003D0u)
#define CTSEL_FRO12M  4u
#define CT0 0x4000C000u
#define CT_IR  (*(volatile uint32_t *)(CT0 + 0x00))
#define CT_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT_PR  (*(volatile uint32_t *)(CT0 + 0x0C))
#define CT_MCR (*(volatile uint32_t *)(CT0 + 0x14))
#define CT_MR0 (*(volatile uint32_t *)(CT0 + 0x18))

/* ---- SysTick (on the core clock = the AHB bus clock) ---------------------- */
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

/* ---- SCG0 (PLL0 -> 150 MHz main clock) + SYSCON AHBCLKDIV ----------------- */
#define SCG0 0x40044000u
#define SCG_FIRCCSR  (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_APLLCSR  (*(volatile uint32_t *)(SCG0 + 0x500))
#define SCG_APLLCTRL (*(volatile uint32_t *)(SCG0 + 0x504))
#define SCG_APLLNDIV (*(volatile uint32_t *)(SCG0 + 0x50C))
#define SCG_APLLMDIV (*(volatile uint32_t *)(SCG0 + 0x510))
#define SCG_APLLPDIV (*(volatile uint32_t *)(SCG0 + 0x514))
#define SCG_RCCR     (*(volatile uint32_t *)(SCG0 + 0x014))
#define SYSCON_AHBCLKDIV (*(volatile uint32_t *)0x40000380u)
static void clock_init_150m(void)
{
    SCG_FIRCCSR |= 1u;
    SCG_APLLCTRL = 0x020035B0u;
    SCG_APLLNDIV = 8; SCG_APLLMDIV = 50; SCG_APLLPDIV = 1;
    SCG_APLLCSR |= 3u;
    SCG_RCCR = (5u << 24);            /* main clock = PLL0 -> 150 MHz */
}

#define MR0_COUNTS 1200u

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

void cpu0_main(void)
{
    uint32_t div1, div2, ratio;
    int ok = 1;

    LP_CTRL = (1u << 19);
    clock_init_150m();
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = CTSEL_FRO12M;      /* reference on FRO_12M, not the AHB clock */

    puts_("AHBCLKDIV test\r\n");

    /* AHBCLKDIV=0 (÷1): core/SysTick = 150 MHz.  1200 CTIMER ticks @12 MHz = 100us -> 15000. */
    SYSCON_AHBCLKDIV = 0;
    div1 = measure();
    puts_("  AHBCLKDIV=0 (/1, core 150MHz): "); putdec(div1); puts_(" SysTick\r\n");

    /* AHBCLKDIV=1 (÷2): core/SysTick = 75 MHz.  A 75 MHz SysTick counts HALF the ticks over
     * the same fixed CTIMER interval -> ~7500. */
    SYSCON_AHBCLKDIV = 1;
    div2 = measure();
    puts_("  AHBCLKDIV=1 (/2, core 75MHz):  "); putdec(div2); puts_(" SysTick\r\n");

    ok &= (div1 > 14700u && div1 < 15300u);          /* /1: ~15000       */
    ok &= (div2 > 7350u && div2 < 7650u);            /* /2: ~7500 (half) */
    ratio = div1 ? (div2 * 100u) / div1 : 0;          /* x100             */
    puts_("  /2-vs-/1 ratio x100 = "); putdec(ratio); puts_(" (expect 50; ignored AHBCLKDIV = 100)\r\n");
    ok &= (ratio > 45u && ratio < 55u);               /* SysTick clock HALVED (0.5) */

    puts_(ok ? "AHBCLKDIV PASS\r\n" : "AHBCLKDIV FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[60] = { [0] = (vec_t)0x20010000u, [1] = cpu0_main };
