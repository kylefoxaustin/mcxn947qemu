/*
 * MCXN947 the M33 CORE CLOCK (and SysTick) is DERIVED from the SCG, not a fixed constant.
 *
 * Out of reset the SCG main clock is RCCR[SCS]=3 = FRO_HF = 48 MHz, so the core boots at
 * 48 MHz -- exactly as silicon does before BOARD_InitBootClocks.  Bringing up PLL0 and
 * switching the main clock to it raises the core (and SysTick) to 150 MHz.
 *
 * Proof, against an INDEPENDENT real-time reference: the MRT runs on the fixed 150 MHz bus
 * clock, so one MRT one-shot is a fixed wall-clock interval.  Time it with SysTick BEFORE
 * the PLL is configured (SysTick @ 48 MHz reset core) and AFTER (SysTick @ 150 MHz): the
 * SAME interval now costs 150/48 = 3.125x more SysTick ticks, because SysTick's clock moved.
 *
 *   A model that pinned the core clock to a 150 MHz constant (the old board `sysclk`) reads
 *   the SAME count both times -- the reset-vs-configured clock difference is invisible, and a
 *   developer whose un-configured code assumed 150 MHz would be 3.1x wrong on silicon.
 *
 * ⚠ -icount shift=3 REQUIRED: the measurement is in virtual SysTick ticks.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* ---- MRT0 (fixed 150 MHz bus clock -> a real-time reference) -------------- */
#define MRT0      0x40013000u
#define CH(n)     (MRT0 + (n) * 0x10u)
#define CH_INTVAL 0x0
#define CH_CTRL   0x8
#define CH_STAT   0xC
#define INTVAL_LOAD  (1u << 31)
#define CTRL_ONESHOT (1u << 1)
#define STAT_INTFLAG (1u << 0)

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

#define NCOUNTS 200000u

/* Time one MRT one-shot of NCOUNTS against SysTick (which counts DOWN). */
static uint32_t measure(void)
{
    uint32_t t0, t1;

    MEM32(CH(0) + CH_CTRL) = 0;
    MEM32(CH(0) + CH_STAT) = STAT_INTFLAG;
    SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = (1u << 0) | (1u << 2);

    MEM32(CH(0) + CH_CTRL)   = CTRL_ONESHOT;
    t0 = SYST_CVR;
    MEM32(CH(0) + CH_INTVAL) = INTVAL_LOAD | NCOUNTS;
    while (!(MEM32(CH(0) + CH_STAT) & STAT_INTFLAG)) {
    }
    t1 = SYST_CVR;
    MEM32(CH(0) + CH_STAT) = STAT_INTFLAG;
    MEM32(CH(0) + CH_CTRL) = 0;
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

    /* (1) Reset core clock = FRO_HF = 48 MHz.  200000 MRT counts (@150 MHz) measured by a
     *     48 MHz SysTick = 200000 * 48/150 = 64000 ticks. */
    a = measure();
    puts_("  reset core (FRO_HF 48MHz): 200000 MRT counts -> "); putdec(a); puts_(" SysTick\r\n");

    /* (2) Raise the core to 150 MHz via PLL0. */
    clock_init_150m();

    /* (3) Same MRT interval, now measured by a 150 MHz SysTick = 200000 ticks. */
    b = measure();
    puts_("  after PLL0 (150MHz):       200000 MRT counts -> "); putdec(b); puts_(" SysTick\r\n");

    ok &= (a > 62720u && a < 65280u);            /* 48 MHz: ~64000 (200000*48/150)  */
    ok &= (b > 196000u && b < 204000u);          /* 150 MHz: ~200000                */
    /* The SysTick clock jumped 150/48 = 3.125x: the core clock is DERIVED, not pinned. */
    ok &= (b > a * 30u / 10u && b < a * 33u / 10u);

    puts_(ok ? "CORECLK PASS\r\n" : "CORECLK FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[64] = { [0] = (vec_t)0x20010000u, [1] = cpu0_main };
