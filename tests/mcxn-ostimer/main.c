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

/* Arm a match COUNTS ahead and return the SysTick ticks it took to arrive. */
#define COUNTS 100u
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
    ratio_x10 = t_1m ? (t_16k * 10u) / t_1m : 0;
    puts_("  ratio x10 = "); putdec(ratio_x10); puts_(" (expect 625 = 62.5x)\r\n");
    ok &= (ratio_x10 > 594 && ratio_x10 < 656);   /* 62.5x +/- 5% */

    puts_(ok ? "OSTIMER PASS\r\n" : "OSTIMER FAIL\r\n");
    for(;;){}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]=(vec_t)0x20010000u, [1]=cpu0_main, [16+OSEVENT_IRQ]=osevent_handler,
};
