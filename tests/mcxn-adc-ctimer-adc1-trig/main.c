/*
 * THE PER-DESTINATION SELECTOR DIVERGENCE -- one written selector value, two ADCs, two
 * DIFFERENT physical trigger sources.
 *
 * On silicon the INPUTMUX ADC-trigger selectors are per-destination: ADC0_TRIG=8 names
 * CTIMER3 match-3, but ADC1_TRIG=8 names CTIMER3 match-2 (NXP's connection table --
 * kINPUTMUX_Ctimer3M3ToAdc0Trigger vs kINPUTMUX_Ctimer3M2ToAdc1Trigger, both value 8).
 * A shared-selector model that treats "8" as one global source fires the WRONG CTIMER
 * match on ADC1.
 *
 * This test drives CTIMER3 with ONLY match-2 periodic (match-3 never fires), routes BOTH
 * ADC0_TRIG[0] and ADC1_TRIG[0] to selector 8, arms HTEN on both, and asserts:
 *   - ADC1 CONVERTS   (selector 8 -> CTIMER3 M2, which is firing),
 *   - ADC0 does NOT   (selector 8 -> CTIMER3 M3, which is silent),
 *   - ADC0 still CONVERTS on a software trigger (so its silence was ROUTING, not a dead
 *     ADC0 -- the negative control has a positive confirmation).
 *
 * The CPU never hardware-triggers a conversion; CTIMER3's match-2 does.  Readout is by
 * polling FCTRL[FCOUNT] (this test is about WHICH match reaches WHICH ADC, not the DMA
 * path -- that is covered by mcxn-adc-ctimer-trig).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4 + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

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

static void puthex(uint32_t v)
{
    const char *H = "0123456789ABCDEF";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        putc_(H[(v >> i) & 0xF]);
    }
}

/* ---- clocks (CTIMER3 <- FRO_HF 48 MHz) ----------------------------------- */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define FIRCCSR_FIRCEN (1u << 0)
#define CTIMER3CLKSEL (*(volatile uint32_t *)0x40000278u)   /* SYSCON CTIMERCLKSEL[3] */
#define CTIMER3CLKDIV (*(volatile uint32_t *)0x400003DCu)   /* SYSCON CTIMERCLKDIV[3] */
#define SEL_FROHF   3u

/* ---- CTIMER3 (base 0x4000F000) ------------------------------------------- */
#define CT3 0x4000F000u
#define CT3_TCR (*(volatile uint32_t *)(CT3 + 0x04))
#define CT3_MCR (*(volatile uint32_t *)(CT3 + 0x14))
#define CT3_MR2 (*(volatile uint32_t *)(CT3 + 0x18 + 2 * 4))
#define TCR_RUN 1u
#define TCR_RST 2u
#define MCR_RST2 (1u << 7)          /* reset TC on match 2 -> periodic M2 */
#define MR2_COUNTS 300u

/* ---- ADCs (same register layout; ADC0 @0x4010D000, ADC1 @0x4010E000) ----- */
#define ADC0 0x4010D000u
#define ADC1 0x4010E000u
#define ADC_CTRL(b)   (*(volatile uint32_t *)((b) + 0x010))
#define ADC_SWTRIG(b) (*(volatile uint32_t *)((b) + 0x034))
#define ADC_TCTRL0(b) (*(volatile uint32_t *)((b) + 0x0A0))
#define ADC_FCTRL0(b) (*(volatile uint32_t *)((b) + 0x0E0))
#define ADC_CMDL0(b)  (*(volatile uint32_t *)((b) + 0x100))
#define ADC_CMDH0(b)  (*(volatile uint32_t *)((b) + 0x104))
#define ADC_RESFIFO0(b) (*(volatile uint32_t *)((b) + 0x300))
#define CTRL_ADCEN   (1u << 0)
#define TCTRL_HTEN   (1u << 0)
#define FCOUNT_MASK  0x1Fu

/* INPUTMUX0: ADC0_TRIG[0] @0x280, ADC1_TRIG[0] @0x2C0. */
#define INPUTMUX0    0x40006000u
#define ADC0_TRIG0   (*(volatile uint32_t *)(INPUTMUX0 + 0x280))
#define ADC1_TRIG0   (*(volatile uint32_t *)(INPUTMUX0 + 0x2C0))
#define TRIG_SEL_8   8u    /* ADC0: CTIMER3 M3 ; ADC1: CTIMER3 M2 -- SAME value, per NXP */

static void adc_init(uint32_t b)
{
    ADC_CTRL(b)   = CTRL_ADCEN;
    ADC_CMDL0(b)  = 0;                       /* channel 0 */
    ADC_CMDH0(b)  = 0;
    ADC_TCTRL0(b) = (1u << 24) | TCTRL_HTEN; /* trigger 0 -> command 1, HW-trigger armed */
}

static uint32_t adc_fcount(uint32_t b)
{
    return ADC_FCTRL0(b) & FCOUNT_MASK;
}

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;
    uint32_t c0, c1, res1;

    LP_CTRL = CTRL_TE;
    puts_("ADC-CTIMER-ADC1-TRIG test\r\n");

    /* CTIMER3 on FRO_HF so it actually counts. */
    SCG_FIRCCSR   = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    CTIMER3CLKDIV = 0;
    CTIMER3CLKSEL = SEL_FROHF;

    adc_init(ADC0);                          /* sel 8 -> CTIMER3 M3 (silent)   */
    adc_init(ADC1);                          /* sel 8 -> CTIMER3 M2 (firing)   */
    ADC0_TRIG0 = TRIG_SEL_8;
    ADC1_TRIG0 = TRIG_SEL_8;

    /* CTIMER3: ONLY match-2 active + periodic (reset on M2).  Match-3 never fires. */
    CT3_MR2 = MR2_COUNTS;
    CT3_MCR = MCR_RST2;
    CT3_TCR = TCR_RST;
    CT3_TCR = TCR_RUN;

    for (d = 0; d < 800000; d++) {           /* let match-2 fire many times    */
    }
    CT3_TCR = 0;                             /* stop                           */

    c0 = adc_fcount(ADC0);
    c1 = adc_fcount(ADC1);
    puts_("  ADC0 FCOUNT = "); puthex(c0);
    puts_("  ADC1 FCOUNT = "); puthex(c1); puts_("\r\n");

    /* ADC1 (selector 8 -> M2, firing) MUST have converted. */
    if (c1 == 0) {
        puts_("  FAIL: ADC1 did not convert on CTIMER3 M2 (selector 8)\r\n");
        ok = 0;
    }
    /* ADC0 (selector 8 -> M3, silent) MUST NOT have converted. */
    if (c0 != 0) {
        puts_("  FAIL: ADC0 converted on selector 8 -- it should route to the SILENT M3\r\n");
        ok = 0;
    }

    /* ADC1 result must be a real, VALID conversion. */
    res1 = ADC_RESFIFO0(ADC1);
    ok &= !!(res1 & (1u << 31));              /* RESFIFO[VALID] */
    puts_("  ADC1 result = "); puthex(res1); puts_("\r\n");

    /*
     * NEGATIVE-CONTROL POSITIVE CHECK: ADC0's silence must be ROUTING, not a dead ADC0.
     * A software trigger MUST make ADC0 convert -- proving selector 8 reached the silent
     * M3, not that ADC0 was broken.
     */
    ADC_SWTRIG(ADC0) = 1u;                    /* trigger 0 by software */
    if (adc_fcount(ADC0) == 0) {
        puts_("  FAIL: ADC0 will not convert even on a software trigger (dead ADC0?)\r\n");
        ok = 0;
    } else {
        puts_("  ADC0 converts on SWTRIG -> its HW silence was routing (correct)\r\n");
    }

    puts_(ok ? "ADC1-TRIG PASS\r\n" : "ADC1-TRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
