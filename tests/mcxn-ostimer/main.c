/*
 * MCXN947 OSTIMER: the SELECTOR DECIDES THE RATE.
 *
 * ⭐ WHAT THIS TEST USED TO BE, AND WHY IT PROVED NOTHING ABOUT THE CLOCK.
 *
 * It armed a match "~50ms ahead at 1 MHz" and waited for the interrupt.  It NEVER
 * SELECTED A CLOCK SOURCE -- and it did not have to, because the model HARDCODED
 * 1 MHz:
 *
 *     return hz ? hz : OSTIMER_HZ;      -- OSTIMER_HZ = 1000000
 *
 * The device declared a Clock input and THE SoC NEVER CONNECTED IT, so `hz` was
 * always 0 and the fallback fired every single time.
 *
 *     ⭐ THE FALLBACK WAS THE CAMOUFLAGE.  The model had exactly the right structure
 *        -- a Clock input, ready to be driven -- and a default that made the missing
 *        wiring invisible.  SYSCON[OSTIMERCLKSEL], the register that CHOOSES the
 *        rate, was consumed by NOTHING.  A guest selecting the 16 kHz source got a
 *        timer running at 1 MHz -- SIXTY-TWO TIMES TOO FAST -- and nothing said a
 *        word.
 *
 * And the test was complicit: it did something NO REAL FIRMWARE DOES -- ran a timer
 * without ever attaching a clock to it (the SDK calls CLOCK_AttachClk) -- and the
 * model's default quietly rescued it.  When the default was removed, THIS TEST WAS
 * THE FIRST THING TO FAIL, which is exactly what should have happened.
 *
 * SO IT NOW SWEEPS THE AXIS IT CLAIMS.  Per the SDK (fsl_clock.c):
 *
 *     switch (SYSCON->OSTIMERCLKSEL) {
 *         case 0U: CLOCK_GetClk16KFreq()  ->    16 000 Hz
 *         case 1U: CLOCK_GetOsc32KFreq()  ->    32 768 Hz
 *         case 2U: CLOCK_GetClk1MFreq()   -> 1 000 000 Hz
 *         default: 0U                     -> NO CLOCK  (and 3 IS THE RESET VALUE)
 *     }
 *
 * The golden is a RATIO, not an absolute: the same match distance must take
 * 1000000/16000 = 62.5x LONGER on the 16 kHz source than on the 1 MHz one.  That
 * ratio is exact and comes from the SDK's own constants -- it does not depend on our
 * assumed system clock at all.  A model that ignores the selector produces 1.0 here
 * and fails.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t*)(LP+0x14))
#define LP_CTRL (*(volatile uint32_t*)(LP+0x18))
#define LP_DATA (*(volatile uint32_t*)(LP+0x1C))

static void putc_(char c){ while(!(LP_STAT&(1u<<23))){} LP_DATA=(uint8_t)c; }
static void puts_(const char*s){ while(*s) putc_(*s++); }
static void putdec(uint32_t v){ char b[12]; int i=0;
    if(!v){putc_('0');return;} while(v){b[i++]=(char)('0'+v%10); v/=10;} while(i--) putc_(b[i]); }

#define OST 0x40049000u
#define EVTL   (*(volatile uint32_t*)(OST+0x00))
#define MATL   (*(volatile uint32_t*)(OST+0x10))
#define MATH   (*(volatile uint32_t*)(OST+0x14))
#define OSCTRL (*(volatile uint32_t*)(OST+0x1C))

/* SYSCON[OSTIMERCLKSEL] — the register that DECIDES the rate. */
#define OSTIMERCLKSEL (*(volatile uint32_t*)0x400005E0u)
#define SEL_16K  0u
#define SEL_1M   2u
#define SEL_NONE 3u    /* the RESET value: no source selected */

/* SysTick — an independent time base that knows nothing about the OSTIMER. */
#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

#define NVIC_ISER(n) (*(volatile uint32_t*)(0xE000E100u + 4*(n)))
#define OSEVENT_IRQ 57

static uint32_t g2b(uint32_t g){ uint32_t b=g; while(g>>=1) b^=g; return b; }
static uint32_t b2g(uint32_t n){ return n ^ (n>>1); }

static volatile int fired;
static volatile uint32_t t_fire;

void osevent_handler(void){ OSCTRL = OSCTRL | 1u; t_fire = SYST_CVR; fired = 1; }

/*
 * Arm a match COUNTS ahead and return the SysTick ticks it took to arrive.
 *
 * ⚠ THIS MEASUREMENT WAS FLAKY, AND A +/-5% WINDOW WAS HIDING IT.
 *
 *   Ten runs under -icount gave 624, 624, 630 -- and -icount is supposed to make virtual
 *   time REPRODUCIBLE.  The raw counts said why:
 *
 *       1 MHz : 14861 SysTick ticks   (golden 15000 -- SHORT BY 139)
 *       16 kHz: 937511               (golden 937500 -- essentially exact)
 *
 *   139 ticks at 150 MHz is ~0.93 us: ALMOST EXACTLY ONE 1 MHz TIMER TICK.  We arm the
 *   match by reading the FREE-RUNNING counter and adding COUNTS -- so the interval we
 *   measure is (COUNTS - phase) ticks, where `phase` is wherever in the current tick we
 *   happened to arm.  The phase error is at most ONE TICK, i.e. 1/COUNTS of the interval.
 *   With COUNTS = 100 that is 1% -- and 1% of 62.5 is exactly the 624-vs-630 we saw.
 *   At 16 kHz one tick is 9375 SysTick ticks, so the same absolute error is invisible
 *   against a 937500-tick interval.  Hence the asymmetry, and hence the flake.
 *
 *     ⭐ A WIDE TOLERANCE DOES NOT MAKE A NOISY MEASUREMENT ACCURATE.
 *       IT MAKES THE NOISE INVISIBLE.  (rt1180emulator: A RANGE IS NOT A GOLDEN.)
 *
 *   And this is the delay-loop crutch wearing a different hat: a flaky test is a bug you
 *   have agreed to see only SOMETIMES, and the +/-5% window WAS the camouflage.
 *
 *   The phase error is bounded by one tick, so it shrinks as 1/COUNTS.
 *
 * ⚠ AND MY FIRST FIX WAS COUNTS = 2000, WHICH BROKE IT WORSE: SYSTICK IS 24-BIT.  At
 *   16 kHz one timer tick is 9375 SysTick ticks, so 2000 of them need 18,750,000 --
 *   ABOVE THE 16,777,215 THE COUNTER CAN HOLD.  It WRAPPED, and the ratio came out 65.
 *
 *     ⭐ A MEASUREMENT THAT OVERFLOWED IS NOT A SMALL MEASUREMENT. IT IS A WRONG ONE.
 *
 *   I removed a phase error and introduced an overflow, and the only reason I caught it
 *   is that I had JUST tightened the window -- the old +/-5% would have been fooled by 65
 *   too, but I would have been looking at a number I had stopped questioning.
 *
 *   COUNTS = 1500 keeps the 16 kHz interval at 14,062,500 ticks (inside the 24-bit
 *   counter, with margin), puts the phase error at 1/1500 = 0.07% per measurement
 *   (~0.1% on the ratio), and lets the window close from +/-5% to +/-0.5% -- ten times
 *   tighter, and now it CATCHES a selector or divider that is even 1% wrong.
 *
 *   The overflow is GUARDED, not assumed away: an interval that does not fit is a FAIL
 *   with its own message, never a quietly-wrong ratio.
 */
#define SYST_MAX_SAFE 0xF00000u   /* leave headroom below the 24-bit wrap */
#define COUNTS 1500u
static uint32_t measure(uint32_t sel)
{
    uint32_t t0, cur, g;

    OSCTRL = 0;                       /* disable while re-clocking */
    OSTIMERCLKSEL = sel;              /* <-- what CLOCK_AttachClk() does */
    fired = 0;

    cur = g2b(EVTL);
    g   = b2g(cur + COUNTS);
    MATL = g; MATH = 0;

    t0 = SYST_CVR;
    OSCTRL = 2u;                      /* INTENA */
    while (!fired) {
    }
    return (t0 - t_fire) & SYST_MASK; /* SysTick counts DOWN */
}

void cpu0_main(void)
{
    uint32_t t_1m, t_16k, ratio_x10;
    int ok = 1;

    LP_CTRL = (1u<<19);
    puts_("OSTIMER test\r\n");

    SYST_RVR = SYST_MASK;
    SYST_CVR = 0;
    SYST_CSR = (1u<<0) | (1u<<2);     /* enable, processor clock */

    NVIC_ISER(OSEVENT_IRQ/32) = (1u << (OSEVENT_IRQ%32));
    __asm__ volatile("cpsie i");

    t_1m  = measure(SEL_1M);
    puts_("  OSTIMERCLKSEL=2 (1 MHz)  : "); putdec(t_1m);  puts_(" SysTick ticks\r\n");

    t_16k = measure(SEL_16K);
    puts_("  OSTIMERCLKSEL=0 (16 kHz) : "); putdec(t_16k); puts_(" SysTick ticks\r\n");

    /*
     * THE GOLDEN, from the SDK's own constants: 1000000 / 16000 = 62.5x.
     * A model that IGNORES the selector -- which is what this one did -- produces a
     * ratio of 1.0 and fails right here.  The old test could not have noticed,
     * because it never changed the selector and never measured anything.
     */
    /* ⭐ AN OVERFLOWED MEASUREMENT IS NOT A SMALL ONE -- IT IS A WRONG ONE.  Refuse it. */
    if (t_16k >= SYST_MAX_SAFE || t_1m >= SYST_MAX_SAFE) {
        puts_("  SysTick OVERFLOW: the interval does not fit in 24 bits\r\n");
        ok = 0;
    }

    ratio_x10 = t_1m ? (t_16k * 10u) / t_1m : 0;
    puts_("  ratio x10 = "); putdec(ratio_x10); puts_(" (expect 625 = 62.5x)\r\n");
    ok &= (ratio_x10 > 621 && ratio_x10 < 629);   /* 62.5x +/- 0.5% -- see COUNTS */

    puts_(ok ? "OSTIMER PASS\r\n" : "OSTIMER FAIL\r\n");
    for(;;){}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]=(vec_t)0x20010000u, [1]=cpu0_main, [16+OSEVENT_IRQ]=osevent_handler,
};
