/*
 * MCXN947 CTIMER: THE CLOCK SELECTOR DECIDES THE RATE.
 *
 * ⭐ WHAT THIS TEST USED TO BE, AND THE BUG IT COULD NOT SEE.
 *
 * It armed a periodic match and counted interrupts ("irq_count >= 5 ? PASS : FAIL"),
 * and it NEVER ATTACHED A CLOCK -- something NO REAL FIRMWARE DOES.  Every stock NXP
 * example calls CLOCK_AttachClk(kFRO_HF_to_CTIMER0) first, and then computes its
 * match values from CLOCK_GetCTimerClkFreq().
 *
 * It did not have to, because the model hardwired the CTIMER to sysclk and IGNORED
 * SYSCON[CTIMERCLKSEL] completely:
 *
 *     return hz ? hz : 150000000;   -- "fallback if the clock tree isn't driven"
 *
 * So firmware that was told "you are on FRO_HF, 48 MHz" got a timer ticking at
 * 150 MHz.  EVERY CTIMER DELAY WAS 3.1x TOO SHORT, SILENTLY.  Measured with SysTick
 * before the fix: 12011 ticks where the SDK's own arithmetic expects 150000.
 *
 *     ⭐ THE FALLBACK WAS THE CAMOUFLAGE -- and it said so in its own comment.  It
 *        announced that the clock tree might not be driven, and then made that fact
 *        invisible.  A `?:` is not a safety net.  It is a place for a bug to live
 *        where no test will ever look.
 *
 * And an interrupt-counting test cannot see ANY of this, because THE INTERRUPTS STILL
 * ARRIVE.  Only at the wrong time.
 *
 * SO IT NOW SWEEPS THE AXIS IT CLAIMS.  Three points, and the goldens are RATIOS taken
 * from the SDK's own source rates -- they do not depend on our assumed sysclk at all:
 *
 *   1. CTIMERCLKSEL = 4  -> FRO12M, 12 MHz
 *   2. CTIMERCLKSEL = 3  -> FRO_HF, 48 MHz   => MUST BE EXACTLY 4x FASTER
 *   3. FRO_HF, CTIMERCLKDIV = 3 (divide by 4) => MUST BE EXACTLY BACK TO (1)
 *
 * A model that ignores the SELECTOR fails (2).  A model that ignores the DIVIDER
 * fails (3).  Neither could fail the old test.
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
static void putdec(uint32_t v){ char b[12]; int i=0; if(!v){putc_('0');return;}
    while(v){b[i++]=(char)('0'+v%10); v/=10;} while(i--) putc_(b[i]); }

/* SCG0 — the source clocks.  Addresses from CMSIS, not from memory. */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t*)(SCG0 + 0x300))
#define SCG_FIRCCFG (*(volatile uint32_t*)(SCG0 + 0x308))
#define FIRCCSR_FIRCEN (1u<<0)

/* SYSCON — CMSIS: CTIMERCLKSEL[5] @0x26C step 4; CTIMERCLKDIV[5] @0x3D0 step 4. */
#define CTIMER0CLKSEL (*(volatile uint32_t*)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t*)0x400003D0u)
#define SEL_FROHF   3u      /* CLOCK_AttachClk(kFRO_HF_to_CTIMER0)  -> 48 MHz */
#define SEL_FRO12M  4u      /* CLOCK_AttachClk(kFRO12M_to_CTIMER0) -> 12 MHz */

/* CTIMER0 — CMSIS: CTIMER0_BASE = 0x4000C000. */
#define CT0 0x4000C000u
#define CT_IR  (*(volatile uint32_t*)(CT0+0x00))
#define CT_TCR (*(volatile uint32_t*)(CT0+0x04))
#define CT_PR  (*(volatile uint32_t*)(CT0+0x0C))
#define CT_MCR (*(volatile uint32_t*)(CT0+0x14))
#define CT_MR0 (*(volatile uint32_t*)(CT0+0x18))

#define SYST_CSR (*(volatile uint32_t*)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t*)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t*)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

#define MR0_COUNTS 1200u

/* Time MR0_COUNTS CTIMER ticks against SysTick, which knows nothing about CTIMER. */
static uint32_t measure(void)
{
    uint32_t t0, t1;

    CT_TCR = 2;              /* hold in reset */
    CT_PR  = 0;              /* prescaler /1 -- swept separately by CTIMERCLKDIV */
    CT_MR0 = MR0_COUNTS;
    CT_MCR = 1u<<0;          /* flag IR[0] on the MR0 match */
    CT_IR  = 0xFF;           /* W1C the flags */

    t0 = SYST_CVR;
    CT_TCR = 1;              /* run */
    while (!(CT_IR & 1u)) {
    }
    t1 = SYST_CVR;
    CT_TCR = 0;
    return (t0 - t1) & SYST_MASK;    /* SysTick counts DOWN */
}

void cpu0_main(void)
{
    uint32_t t_12m, t_hf, t_hf_div4, r_x100;
    int ok = 1;

    LP_CTRL = (1u<<19);
    puts_("CTIMER test\r\n");

    /* Enable the FIRC, exactly as BOARD_InitBootClocks does -- FRO_HF is 0 Hz until
     * FIRCCSR[FIRCEN] is set (fsl_clock.c: CLOCK_GetFroHfFreq). RANGE=0 -> 48 MHz. */
    SCG_FIRCCFG = 0;
    SCG_FIRCCSR = SCG_FIRCCSR | FIRCCSR_FIRCEN;

    SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = (1u<<0)|(1u<<2);

    /* (1) FRO12M, 12 MHz, divider 1. */
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = SEL_FRO12M;
    t_12m = measure();
    puts_("  SEL=4 FRO12M (12 MHz)      : "); putdec(t_12m); puts_(" SysTick ticks\r\n");

    /* (2) FRO_HF, 48 MHz, divider 1 -> MUST be exactly 4x faster. */
    CTIMER0CLKSEL = SEL_FROHF;
    t_hf = measure();
    puts_("  SEL=3 FRO_HF (48 MHz)      : "); putdec(t_hf);  puts_(" SysTick ticks\r\n");

    /* (3) FRO_HF divided by 4 -> MUST be back at (1). */
    CTIMER0CLKDIV = 3;                 /* DIV field: divide by DIV+1 */
    t_hf_div4 = measure();
    puts_("  SEL=3 FRO_HF, CLKDIV=/4    : "); putdec(t_hf_div4); puts_(" SysTick ticks\r\n");

    /* THE SELECTOR: 48/12 = 4.00x exactly.  A model that ignores it gives 1.00x. */
    r_x100 = t_hf ? (t_12m * 100u) / t_hf : 0;
    puts_("  FRO12M/FRO_HF ratio x100   : "); putdec(r_x100);
    puts_(" (expect 400 = 4.00x)\r\n");
    ok &= (r_x100 > 380 && r_x100 < 420);

    /* THE DIVIDER: /4 of 48 MHz IS 12 MHz.  A model that ignores CLKDIV gives 4.00x. */
    r_x100 = t_hf_div4 ? (t_12m * 100u) / t_hf_div4 : 0;
    puts_("  FRO12M/(FRO_HF/4) ratio x100: "); putdec(r_x100);
    puts_(" (expect 100 = 1.00x)\r\n");
    ok &= (r_x100 > 95 && r_x100 < 105);

    puts_(ok ? "CTIMER PASS\r\n" : "CTIMER FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[60] = { [0]=(vec_t)0x20010000u, [1]=cpu0_main };
