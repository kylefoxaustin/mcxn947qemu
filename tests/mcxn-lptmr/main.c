/*
 * MCXN947 LPTMR runs on its PSR[PCS]-SELECTED LOW-POWER clock, not the bus clock.
 *
 * ⭐ THE BUG THIS CATCHES: the LPTMR ignored PSR[PCS] and ran on the 150 MHz sysclk -- but
 *   the LPTMR is a LOW-POWER timer whose max is 25 MHz, and PSR[PCS] selects one of four
 *   low-power clocks (RM rev 7, Table 463):
 *     00 FRO_12M (12 MHz)   01 FRO_16K (16 kHz)   10 32K_CLK (32.768 kHz)   11 OSC_SYS
 *   A driver that selected FRO_12M got a timer running 12.5x too fast, silently.
 *
 * Measured against SysTick -- an Arm core timer, independent of the LPTMR model -- with the
 * core clock brought to a known 150 MHz.  Two checks, goldens from the RM/SDK source rates
 * (never from the model):
 *   1. PCS=00 FRO_12M: 1 LPTMR count = 150M/12M = 12.5 SysTick ticks.  The old 150 MHz model
 *      gives 1.0 -- caught.
 *   2. PCS=00 vs PCS=10 (32K): the SAME count takes 12M/32768 = ~366x MORE ticks at 32K.  A
 *      model that ignored PCS reports the same rate for both -> ratio 1.0 -> caught.
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

/* ---- LPTMR0 -------------------------------------------------------------- */
#define LPTMR0 0x4004A000u
#define LPT_CSR (*(volatile uint32_t *)(LPTMR0 + 0x0))
#define LPT_PSR (*(volatile uint32_t *)(LPTMR0 + 0x4))
#define LPT_CMR (*(volatile uint32_t *)(LPTMR0 + 0x8))
#define LPT_CNR (*(volatile uint32_t *)(LPTMR0 + 0xC))
#define CSR_TEN (1u << 0)
#define CSR_TCF (1u << 7)
#define PSR_PBYP (1u << 2)              /* bypass prescaler: count at the source rate */
#define PCS_FRO12M 0u
#define PCS_32K    2u

/* ---- SysTick ------------------------------------------------------------- */
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SYST_MASK 0x00FFFFFFu

/* ---- SCG0: core to 150 MHz for a known SysTick reference ------------------ */
#define SCG0 0x40044000u
#define SCG_FIRCCSR  (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_APLLCSR  (*(volatile uint32_t *)(SCG0 + 0x500))
#define SCG_APLLCTRL (*(volatile uint32_t *)(SCG0 + 0x504))
#define SCG_APLLNDIV (*(volatile uint32_t *)(SCG0 + 0x50C))
#define SCG_APLLMDIV (*(volatile uint32_t *)(SCG0 + 0x510))
#define SCG_APLLPDIV (*(volatile uint32_t *)(SCG0 + 0x514))
#define SCG_RCCR     (*(volatile uint32_t *)(SCG0 + 0x014))
static void clock_init_150m(void)
{
    SCG_FIRCCSR |= 1u;
    SCG_APLLCTRL = 0x020035B0u;
    SCG_APLLNDIV = 8; SCG_APLLMDIV = 50; SCG_APLLPDIV = 1;
    SCG_APLLCSR |= 3u;
    SCG_RCCR = (5u << 24);
}

/* Time <counts> LPTMR ticks (prescaler bypassed) on source <pcs>, in SysTick ticks. */
static uint32_t measure(uint32_t pcs, uint32_t counts)
{
    uint32_t t0, t1;

    LPT_CSR = 0;                                    /* disable */
    LPT_PSR = PSR_PBYP | pcs;                       /* source, no prescale */
    LPT_CMR = counts;                               /* compare at <counts>  */
    LPT_CSR = CSR_TCF;                              /* W1C stale flag */

    SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = (1u << 0) | (1u << 2);
    t0 = SYST_CVR;
    LPT_CSR = CSR_TEN;                              /* run */
    while (!(LPT_CSR & CSR_TCF)) {
    }
    t1 = SYST_CVR;
    LPT_CSR = 0;
    return (t0 - t1) & SYST_MASK;                   /* SysTick counts DOWN */
}

void cpu0_main(void)
{
    uint32_t fro12m, k32, r100, ratio;
    int ok = 1;

    LP_CTRL = (1u << 19);
    clock_init_150m();
    puts_("LPTMR test\r\n");

    /* PCS=00 FRO_12M: 12000 counts @ 12 MHz = 1 ms; SysTick @150 MHz = 12000*12.5 = 150000. */
    fro12m = measure(PCS_FRO12M, 12000u);
    puts_("  PCS=00 FRO_12M: 12000 counts -> "); putdec(fro12m); puts_(" SysTick\r\n");

    /* PCS=10 32K: 100 counts @ 32768 Hz; SysTick = 100 * 150M/32768 = ~457764. */
    k32 = measure(PCS_32K, 100u);
    puts_("  PCS=10 32K:      100 counts -> "); putdec(k32); puts_(" SysTick\r\n");

    /* (1) FRO_12M absolute: 12.5 ticks/count -> 150000 for 12000 counts (within 2%).
     *     The old 150 MHz model gives 12000 (1 tick/count) -- far outside. */
    r100 = fro12m / 120u;                            /* ticks per count, x100 */
    puts_("  FRO_12M ticks/count x100 = "); putdec(r100); puts_(" (expect ~1250)\r\n");
    ok &= (fro12m > 147000u && fro12m < 153000u);

    /* (2) PCS SELECTOR: per-count, 32K is 12M/32768 = ~366x slower than FRO_12M.  Compute the
     *     ratio in one expression to avoid integer-truncation (12000 counts * 32K-drain vs
     *     100 counts * FRO_12M-drain).  A model that ignored PCS gives the same rate for both
     *     -> ratio ~1; the correct model gives ~366.  The band cleanly separates the two. */
    /* (k32/100)/(fro12m/12000) = (k32*12000)/(100*fro12m) = (k32*120)/fro12m, all 32-bit
     * (k32*120 ~ 55M fits; no 64-bit divide, which -nostdlib can't link). */
    ratio = (k32 * 120u) / fro12m;
    puts_("  32K-vs-FRO_12M per-count ratio = "); putdec(ratio); puts_(" (expect ~366)\r\n");
    ok &= (ratio > 330u && ratio < 400u);            /* ~366; nowhere near 1 (ignored PCS) */

    puts_(ok ? "LPTMR PASS\r\n" : "LPTMR FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[60] = { [0] = (vec_t)0x20010000u, [1] = cpu0_main };
