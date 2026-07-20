/*
 * MCXN947 PLL0 is DERIVED — a CTIMER clocked from PLL0 follows the PLL registers.
 *
 * The SCG computes PLL0's output from its APLL NDIV/MDIV/PDIV registers (RM formula
 * Fout = (Fin/N)*M/(2*P)); SYSCON's CTIMER clock mux exposes PLL0 as selector 1
 * (CLOCK_GetCTimerClkFreq case 1 = CLOCK_GetPll0OutFreq).  Before this, that selector
 * reported 0 Hz -- a timer pointed at the PLL simply did not run.
 *
 * This times a fixed CTIMER interval against SysTick (an independent core timer) with
 * PLL0 = 150 MHz, then HALVES the PLL multiplier (M 50 -> 25 => 75 MHz) exactly as
 * firmware reconfigures a PLL, and times the SAME interval again.  The CTIMER is now
 * half as fast, so the interval takes TWICE the SysTick ticks.  A model that reports a
 * constant (or 0) for the PLL source cannot move between the two measurements.
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

/* ---- SCG0: FIRC + PLL0 (APLL) -------------------------------------------- */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_APLLCSR  (*(volatile uint32_t *)(SCG0 + 0x500))
#define SCG_APLLCTRL (*(volatile uint32_t *)(SCG0 + 0x504))
#define SCG_APLLNDIV (*(volatile uint32_t *)(SCG0 + 0x50C))
#define SCG_APLLMDIV (*(volatile uint32_t *)(SCG0 + 0x510))
#define SCG_APLLPDIV (*(volatile uint32_t *)(SCG0 + 0x514))
#define FIRCEN      (1u << 0)
#define APLL_PWR_CLK 0x3u                  /* APLLPWREN | APLLCLKEN */
#define APLLCTRL_SRC_CLK48M 0x020035B0u    /* SOURCE=1 (+ SELI/SELP, no effect on freq) */

/* ---- SYSCON: CTIMER0 clock mux ------------------------------------------- */
#define CTIMER0CLKSEL (*(volatile uint32_t *)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t *)0x400003D0u)
#define CTSEL_PLL0    1u

/* ---- CTIMER0 ------------------------------------------------------------- */
#define CT0 0x4000C000u
#define CT_IR  (*(volatile uint32_t *)(CT0 + 0x00))
#define CT_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT_PR  (*(volatile uint32_t *)(CT0 + 0x0C))
#define CT_MCR (*(volatile uint32_t *)(CT0 + 0x14))
#define CT_MR0 (*(volatile uint32_t *)(CT0 + 0x18))

/* ---- SysTick (independent reference, still board 150 MHz) ---------------- */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

#define MR0_COUNTS 1200u

/* Time MR0_COUNTS CTIMER ticks against SysTick. */
static uint32_t measure(void)
{
    uint32_t t0, t1;

    CT_TCR = 2; CT_PR = 0; CT_MR0 = MR0_COUNTS; CT_MCR = 1u; CT_IR = 0xFF;
    t0 = SYST_CVR;
    CT_TCR = 1;
    while (!(CT_IR & 1u)) {
    }
    t1 = SYST_CVR;
    CT_TCR = 0;
    return (t0 - t1) & SYST_MASK;
}

void cpu0_main(void)
{
    uint32_t a, b;
    int ok = 1;

    LP_CTRL = (1u << 19);
    puts_("PLL-CTIMER test\r\n");

    SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = (1u << 0) | (1u << 2);

    /* Bring up PLL0 = 150 MHz from the 48 MHz FIRC: N=8, M=50, P=1 -> (48/8)*50/2. */
    SCG_FIRCCSR |= FIRCEN;
    SCG_APLLCTRL = APLLCTRL_SRC_CLK48M;
    SCG_APLLNDIV = 8;
    SCG_APLLMDIV = 50;
    SCG_APLLPDIV = 1;
    SCG_APLLCSR |= APLL_PWR_CLK;

    /* Point CTIMER0 at PLL0 (selector 1), divider 1. */
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = CTSEL_PLL0;

    a = measure();
    puts_("  PLL0=150MHz: 1200 CTIMER ticks -> "); putdec(a); puts_(" SysTick\r\n");

    /* Halve the PLL multiplier: M 50 -> 25 => PLL0 = 75 MHz. */
    SCG_APLLMDIV = 25;

    b = measure();
    puts_("  PLL0=75MHz:  1200 CTIMER ticks -> "); putdec(b); puts_(" SysTick\r\n");

    /*
     * SysTick is the independent reference: the core clock is left on FRO_HF (48 MHz,
     * the reset source -- we do NOT switch the main clock to PLL0, so SysTick stays put
     * while only the PLL-clocked CTIMER moves).  1200 CTIMER ticks at 150 MHz measured by
     * a 48 MHz SysTick = 1200*48/150 = 384; at 75 MHz = 768.
     */
    ok &= (a > 372u && a < 400u);
    ok &= (b > 744u && b < 792u);
    /* And the ratio must be ~2: the CTIMER clock FOLLOWED the PLL. */
    ok &= (b > a * 19u / 10u && b < a * 21u / 10u);

    puts_(ok ? "PLL-CTIMER PASS\r\n" : "PLL-CTIMER FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[60] = { [0] = (vec_t)0x20010000u, [1] = cpu0_main };
