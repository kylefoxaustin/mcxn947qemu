/*
 * MCXN947 SAI (I2S) data-path test — audio words actually move, byte-exact.
 *
 * This block used to move NO DATA: TDR writes were discarded and RDR returned 0,
 * while the capability table advertised a "SAI FIFO data path".  The old version
 * of this test asserted only that a FIFO-request interrupt fired, so replacing RDR
 * with 0xA5A5A5A5 left it green — it could not fail in the dimension it existed to
 * protect.  The claim was retracted; this is the data path being earned back.
 *
 * With SAI_TXD jumpered to SAI_RXD (the "loopback" BOARD option — the MCX N SAI
 * has no loopback register bit, and inventing one would be fabricating silicon), a
 * word written to TDR is serialised out at the configured word rate, arrives on
 * the receive side, and must read back from RDR BIT-FOR-BIT.  The samples are
 * chosen here, so nothing in the model can produce them by accident.
 *
 * It also pins the two things a register file cannot express, and which real
 * firmware must handle:
 *
 *   NEG1  writing a FULL transmit FIFO OVERRUNS — the word is lost and TCSR[FEF]
 *         sets.  A model that accepts an unbounded burst lets firmware push more
 *         audio than the hardware could ever have carried.
 *   NEG2  reading an EMPTY receive FIFO UNDERRUNS — RCSR[FEF] sets, rather than
 *         handing back a plausible zero firmware cannot tell from a real sample
 *         of silence.
 *
 * Prints "SAI PASS" only if every check holds.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE_U (1u << 19)
#define STAT_TDRE (1u << 23)
static void putc_(char c){ while(!(LP_STAT&STAT_TDRE)){} LP_DATA=(uint8_t)c; }
static void puts_(const char*s){ while(*s) putc_(*s++); }
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

/* SysTick — an Arm core timer, independent of every peripheral model. */
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SYST_ENABLE    (1u << 0)
#define SYST_CLKSOURCE (1u << 2)      /* processor clock */
#define SYST_MASK  0x00FFFFFFu        /* 24-bit down-counter */

#define SAI0 0x40106000u
#define SAI_TCSR (*(volatile uint32_t *)(SAI0 + 0x08))
#define SAI_TCR1 (*(volatile uint32_t *)(SAI0 + 0x0C))
#define SAI_TCR2 (*(volatile uint32_t *)(SAI0 + 0x10))
/*
 * ⭐ TCR2[BCD] -- BIT CLOCK DIRECTION.  RM: "0b - Generate externally in Target mode;
 *   1b - Generate internally in Controller mode."
 *
 * THIS TEST USED TO WRITE TCR2 WITHOUT IT -- i.e. IT CONFIGURED THE SAI AS A *TARGET*,
 * WHERE THE BIT CLOCK COMES FROM AN EXTERNAL CODEC -- AND THEN ASSERTED THE CONTROLLER'S
 * DIVIDER MATH.  The model applied the divider unconditionally, so BOTH HALVES OF THE
 * LOOP SHARED THE SAME WRONG BELIEF, and the test was green for it.
 *
 * On this board the SAI drives a TXD->RXD jumper: there is no codec, so THE SAI IS THE
 * CONTROLLER -- nothing else could possibly generate the clock.  Say so.
 *
 * (93emulator: "CHECK BCD BEFORE YOU WRITE ONE LINE OF DIVIDER MATH.  IF IT IS 0, THE
 *  ANSWER IS NOT IN THIS DEVICE." -- their SAI is a bit-clock slave and their RM-correct
 *  divider formula was a fabrication with a citation attached.)
 */
#define TCR2_BCD  (1u << 24)   /* generate the bit clock internally: we are the controller */
#define TCR2_BYP  (1u << 23)   /* bypass the divider: bit clock = divide-by-one of MCLK */
#define SAI_TCR5 (*(volatile uint32_t *)(SAI0 + 0x1C))
#define SAI_TDR0 (*(volatile uint32_t *)(SAI0 + 0x20))
#define SAI_TFR0 (*(volatile uint32_t *)(SAI0 + 0x40))
#define SAI_RCSR (*(volatile uint32_t *)(SAI0 + 0x88))
#define SAI_RCR1 (*(volatile uint32_t *)(SAI0 + 0x8C))
#define SAI_RCR5 (*(volatile uint32_t *)(SAI0 + 0x9C))
#define SAI_RDR0 (*(volatile uint32_t *)(SAI0 + 0xA0))
#define SAI_RFR0 (*(volatile uint32_t *)(SAI0 + 0xC0))

/* TCSR/RCSR (CMSIS I2S_TCSR_*) */
#define CSR_FEF  (1u << 18)   /* FIFO error: overrun / underrun */
#define CSR_FR   (1u << 25)   /* FIFO reset */
#define CSR_EN   (1u << 31)   /* TE / RE */

#define FIFO_DEPTH 8
#define NSAMPLES   6
#define TFR_COUNT(v) (((v) >> 16) & 0xF)

/* ---- SCG0 + SYSCON: the SAI function clock (MCLK) is DERIVED from SAI0CLKSEL ---- */
#define SCG0         0x40044000u
#define SCG_FIRCCSR  (*(volatile uint32_t *)(SCG0 + 0x300))
#define SCG_APLLCSR  (*(volatile uint32_t *)(SCG0 + 0x500))
#define SCG_APLLCTRL (*(volatile uint32_t *)(SCG0 + 0x504))
#define SCG_APLLNDIV (*(volatile uint32_t *)(SCG0 + 0x50C))
#define SCG_APLLMDIV (*(volatile uint32_t *)(SCG0 + 0x510))
#define SCG_APLLPDIV (*(volatile uint32_t *)(SCG0 + 0x514))
#define SCG_RCCR     (*(volatile uint32_t *)(SCG0 + 0x014))
#define FIRCEN       (1u << 0)
#define APLL_PWR_CLK 0x3u
#define APLLCTRL_SRC_CLK48M 0x020035B0u

#define SAI0CLKSEL   (*(volatile uint32_t *)0x40000880u)
#define SAI0CLKDIV   (*(volatile uint32_t *)0x40000888u)
#define SAISEL_PLL0    1u
#define SAISEL_FRO_HF  3u

/* Bring PLL0 to 150 MHz and the core with it (SysTick reference), as firmware does. */
static void clock_init_150m(void)
{
    SCG_FIRCCSR |= FIRCEN;
    SCG_APLLCTRL = APLLCTRL_SRC_CLK48M;
    SCG_APLLNDIV = 8;
    SCG_APLLMDIV = 50;
    SCG_APLLPDIV = 1;
    SCG_APLLCSR |= APLL_PWR_CLK;
    SCG_RCCR = (5u << 24);            /* main clock = PLL0 -> 150 MHz */
}

void cpu0_main(void)
{
    /* Audio words nothing in the model could invent. */
    static const uint32_t samples[NSAMPLES] = {
        0x0000DEAD, 0x0000BEEF, 0x0000CAFE, 0x00001234, 0x0000A5A5, 0x00005A5A
    };
    volatile int d;
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE_U;
    puts_("SAI test\r\n");

    /* The SAI MCLK is now DERIVED from SYSCON SAI0CLKSEL -- with no source selected there is
     * NO MCLK and the SAI cannot clock data.  Bring up PLL0 (150 MHz core for the SysTick
     * reference) and point the SAI function clock at FRO_HF (48 MHz), as firmware does with
     * CLOCK_AttachClk(kFRO_HF_to_SAI0).  The ratio checks below are MCLK-independent, so this
     * only has to make the clock NON-ZERO; the absolute rate is proven separately at the end. */
    clock_init_150m();
    SAI0CLKDIV = 0;
    SAI0CLKSEL = SAISEL_FRO_HF;

    /* 32-bit words at the fastest bit clock: a short word period keeps it brisk. */
    SAI_TCR2 = TCR2_BCD;                 /* DIV = 0                              */
    SAI_TCR5 = (31u << 16);       /* W0W = 31 -> 32-bit words             */
    SAI_RCR5 = (31u << 16);
    SAI_TCR1 = 0;                 /* TFW = 0: ask for data once drained   */
    SAI_RCR1 = 0;                 /* RFW = 0: data ready on the first word */

    SAI_TCSR = CSR_FR;            /* reset both FIFOs */
    SAI_RCSR = CSR_FR;

    ok &= (TFR_COUNT(SAI_TFR0) == 0);
    ok &= (TFR_COUNT(SAI_RFR0) == 0);

    /* --- NEG2: reading an empty receive FIFO must UNDERRUN, not return 0 --- */
    (void)SAI_RDR0;
    ok &= !!(SAI_RCSR & CSR_FEF);
    SAI_RCSR = CSR_FEF;                            /* W1C */
    ok &= !(SAI_RCSR & CSR_FEF);

    /* --- NEG1: writing a full transmit FIFO must OVERRUN, and DROP the word - */
    for (i = 0; i < FIFO_DEPTH; i++) {
        SAI_TDR0 = 0x1000u + i;                    /* transmitter still off */
    }
    ok &= (TFR_COUNT(SAI_TFR0) == FIFO_DEPTH);
    ok &= !(SAI_TCSR & CSR_FEF);
    SAI_TDR0 = 0xFFFFFFFFu;                        /* one too many */
    ok &= !!(SAI_TCSR & CSR_FEF);                  /* overrun flagged */
    ok &= (TFR_COUNT(SAI_TFR0) == FIFO_DEPTH);     /* and NOT accepted */

    SAI_TCSR = CSR_FR;                             /* flush */
    SAI_TCSR = CSR_FEF;                            /* W1C the error */
    ok &= (TFR_COUNT(SAI_TFR0) == 0);
    ok &= !(SAI_TCSR & CSR_FEF);

    /* --- the round trip: TXD is jumpered to RXD, so the words come back ---- */
    SAI_RCSR = CSR_EN;                             /* receiver on */

    for (i = 0; i < NSAMPLES; i++) {
        SAI_TDR0 = samples[i];
    }
    ok &= (TFR_COUNT(SAI_TFR0) == NSAMPLES);

    SAI_TCSR = CSR_EN;                             /* bit clock on */

    /* Let the FIFO drain through the wire into the receiver. */
    for (d = 0; d < 5000000 && TFR_COUNT(SAI_TFR0) != 0; d++) {
    }
    ok &= (TFR_COUNT(SAI_TFR0) == 0);              /* it really drained  */
    ok &= (TFR_COUNT(SAI_RFR0) == NSAMPLES);       /* and really arrived */

    /* Byte-exact, and in order. */
    for (i = 0; i < NSAMPLES; i++) {
        uint32_t got = SAI_RDR0;

        ok &= (got == samples[i]);
    }
    ok &= (TFR_COUNT(SAI_RFR0) == 0);

    SAI_TCSR = 0;                                  /* stop the transmitter */

    /* ------------------------------------------------------------------
     * THE WORD RATE.  The capability table claimed "word rate derived from
     * TCR2[DIV]/TCR5[W0W]" — and NOTHING TESTED IT.  Mutation testing proved
     * it: making the model IGNORE TCR2[DIV] entirely left this test GREEN.
     * The words still moved, still byte-exact, still in order — just at the
     * WRONG RATE, which for an I2S link is the whole point.  (Exactly the bug
     * just found in eFlexPWM, where CTRL[PRSC] was not modelled at all.)
     *
     * Measured against SysTick — an Arm core timer, independent of every
     * peripheral model — under -icount so it is deterministic.
     *
     * ⭐ AND THE CHECK IS MCLK-INDEPENDENT BY CONSTRUCTION.  The bit clock is
     * MCLK / (2 * (DIV + 1)), so the word period must scale EXACTLY with
     * (DIV + 1) — a RATIO, in which the MCLK cancels.  So this verifies the
     * thing the RM actually specifies, WITHOUT depending on the nominal MCLK
     * (which is a documented modelling assumption, the clock tree not being
     * modelled).  A test that divided by the model's own MCLK would prove
     * nothing — that is the trap that hid the PWM tick rate for weeks.
     * ------------------------------------------------------------------ */
    {
        /*
         * SWEEP BOTH AXES.  The period is proportional to BITS * (DIV + 1), so
         * varying DIV alone is a DEGENERATE SHAPE: with W0W frozen at 32 bits,
         * a model that hardcoded `bits = 32` is BIT-IDENTICAL and the test
         * cannot see it.  (Mutation testing caught exactly that here — the same
         * failure as testing a matrix engine only on square matrices, where all
         * three LENGTH fields are equal and a dimension swap changes nothing.)
         *
         * Rows: baseline · double the divider · halve the word length.
         * Expected period is relative to row 0 and MCLK-INDEPENDENT.
         */
        static const uint8_t divs[3] = { 0,  1,  0 };
        static const uint8_t w0w [3] = { 31, 31, 15 };  /* 32, 32, 16 bits */
        static const uint8_t mul [3] = { 2,  4,  1 };   /* period x mul/2   */
        uint32_t drain[3];
        int k;

        SYST_RVR = SYST_MASK;
        SYST_CVR = 0;
        SYST_CSR = SYST_ENABLE | SYST_CLKSOURCE;

        for (k = 0; k < 3; k++) {
            uint32_t t0, t1;

            SAI_TCSR = 0;                       /* transmitter off       */
            SAI_TCSR = CSR_FR;                  /* flush the FIFO        */
            SAI_RCSR = CSR_FR;
            SAI_TCR2 = TCR2_BCD | divs[k];                 /* bit-clock divider     */
            SAI_TCR5 = ((uint32_t)w0w[k] << 16); /* word length           */
            SAI_RCR5 = ((uint32_t)w0w[k] << 16);

            for (i = 0; i < FIFO_DEPTH; i++) {
                SAI_TDR0 = 0x2000u + i;
            }
            ok &= (TFR_COUNT(SAI_TFR0) == FIFO_DEPTH);

            t0 = SYST_CVR;
            SAI_TCSR = CSR_EN;                  /* bit clock on -> drain */
            for (d = 0; d < 200000000 && TFR_COUNT(SAI_TFR0) != 0; d++) {
            }
            t1 = SYST_CVR;
            ok &= (TFR_COUNT(SAI_TFR0) == 0);

            drain[k] = (t0 - t1) & SYST_MASK;   /* SysTick counts DOWN   */
            SAI_TCSR = 0;
        }

        /*
         * ⭐ AND THE TWO GATES THE DIVIDER SITS BEHIND, WHICH THIS TEST NEVER TOUCHED.
         *
         * ① TARGET MODE (TCR2[BCD] = 0): the RM says the bit clock is "generated
         *    externally".  On this board there is no codec -- the SAI drives a TXD->RXD
         *    jumper -- so in Target mode THERE IS NO BIT CLOCK AT ALL and nothing may be
         *    clocked out.  The model used to divide MCLK anyway and drain the FIFO
         *    happily, producing a word rate the hardware could never generate.
         *
         *    ⚠ AND THIS TEST CONFIGURED TARGET MODE AND THEN ASSERTED CONTROLLER MATH.
         *      Both halves of the loop shared the same wrong belief; that is why it was
         *      green.  Now: a Target-mode transmitter must NOT drain.
         *
         * ② BYP (TCR2[BYP] = 1): "bypasses the bit clock divider ... divide-by-one".
         *    With DIV=15 and BYP set, the word must come out at the BYPASSED (fast)
         *    rate, not the divided one -- so the drain must be much QUICKER than row 2
         *    (DIV=15) even though the divider says otherwise.
         */
        SAI_TCSR = 0;
        SAI_TCSR = CSR_FR;
        SAI_RCSR = CSR_FR;
        SAI_TCR2 = 0;                           /* BCD = 0: TARGET mode -- no clock */
        SAI_TCR5 = ((uint32_t)15 << 16);
        for (i = 0; i < FIFO_DEPTH; i++) {
            SAI_TDR0 = 0x3000u + i;
        }
        SAI_TCSR = CSR_EN;                      /* "enable" a transmitter with no clock */
        for (d = 0; d < 2000000; d++) {
        }
        puts_("  TARGET mode (BCD=0): TX FIFO still holds ");
        putdec(TFR_COUNT(SAI_TFR0));
        puts_(" words (must be ");
        putdec(FIFO_DEPTH);
        puts_(" -- no clock, no data)\r\n");
        ok &= (TFR_COUNT(SAI_TFR0) == FIFO_DEPTH);   /* NOTHING may be clocked out */
        SAI_TCSR = 0;

        {
            uint32_t t0, t1, byp_drain, div15_drain;
            int r;

            /*
             * ⭐ THE ONLY BASELINE THAT PROVES BYP DOES ANYTHING IS *THE SAME DIV WITH
             *   BYPASS OFF*.  My first attempt compared BYP(DIV=15) against the sweep's
             *   row 2 -- which is DIV=**0** -- so it was really measuring divide-by-1 vs
             *   divide-by-2, got exactly 2.0x, and I nearly "fixed" a CORRECT MODEL
             *   because my test had the wrong baseline.
             *
             *     ⭐ A TEST WITH THE WRONG BASELINE INDICTS THE MODEL FOR ITS OWN ERROR.
             *
             *   Same DIV, bypass on vs off: divide-by-1 against divide-by-32.  Anything
             *   less than a large ratio means BYP is being ignored.
             */
            for (r = 0; r < 2; r++) {
                SAI_TCSR = 0;
                SAI_TCSR = CSR_FR;
                SAI_RCSR = CSR_FR;
                SAI_TCR2 = TCR2_BCD | (r ? TCR2_BYP : 0u) | 15u;   /* DIV=15 both times */
                SAI_TCR5 = ((uint32_t)15 << 16);
                SAI_RCR5 = ((uint32_t)15 << 16);
                for (i = 0; i < FIFO_DEPTH; i++) {
                    SAI_TDR0 = 0x4000u + i;
                }
                t0 = SYST_CVR;
                SAI_TCSR = CSR_EN;
                for (d = 0; d < 200000000 && TFR_COUNT(SAI_TFR0) != 0; d++) {
                }
                t1 = SYST_CVR;
                SAI_TCSR = 0;
                if (r) {
                    byp_drain = (t0 - t1) & SYST_MASK;
                } else {
                    div15_drain = (t0 - t1) & SYST_MASK;
                }
            }

            puts_("  DIV=15: drain ");
            putdec(div15_drain);
            puts_(" ticks;  DIV=15 + BYP: ");
            putdec(byp_drain);
            puts_(" ticks (bypass = divide-by-1 vs divide-by-32)\r\n");
            /* divide-by-32 vs divide-by-1.  Demand at least 8x -- generous, because the
             * polling loop's own overhead floors the bypassed measurement. */
            ok &= (byp_drain * 8u < div15_drain);
        }

        /* Period must scale as BITS * (DIV+1), relative to row 0.  mul/2 is
         * that ratio: row1 = x2 (divider), row2 = x0.5 (half the word). */
        for (k = 1; k < 3; k++) {
            uint32_t want = drain[0] * mul[k] / 2u;
            uint32_t got  = drain[k];
            uint32_t diff = (got > want) ? (got - want) : (want - got);

            puts_("  DIV="); putdec(divs[k]);
            puts_(" bits="); putdec((uint32_t)w0w[k] + 1);
            puts_(" drain="); putdec(got);
            puts_(" expected "); putdec(want);
            puts_("\r\n");

            ok &= (diff * 100 <= want);         /* within 1% */
        }
    }

    /* ------------------------------------------------------------------------------------
     * ⭐ THE SOURCE-DERIVATION CHECK — the MCLK FOLLOWS SAI0CLKSEL.  The ratio sweep above
     *   cancels MCLK, so it CANNOT catch a model that ignores the selector and uses a
     *   constant.  This does: measure the SAME word config with the SAI clock on FRO_HF
     *   (48 MHz) and on PLL0 (150 MHz).  Word period = bits*2(DIV+1)/MCLK, so the FRO_HF
     *   drain must take 150/48 = 3.125x MORE SysTick ticks (SysTick is on the 150 MHz core).
     *   The golden 150/48 is from the SDK's source rates, NOT the model -- a model that
     *   pinned MCLK to a constant gives ratio 1.0 and FAILS.
     * ------------------------------------------------------------------------------------ */
    {
        uint32_t drain_frohf, drain_pll0;
        uint32_t t0, t1, r100;
        int sel;

        SYST_RVR = SYST_MASK; SYST_CVR = 0; SYST_CSR = SYST_ENABLE | SYST_CLKSOURCE;

        for (sel = 0; sel < 2; sel++) {
            SAI0CLKSEL = sel ? SAISEL_PLL0 : SAISEL_FRO_HF;
            SAI_TCSR = 0;
            SAI_TCSR = CSR_FR;
            SAI_RCSR = CSR_FR;
            SAI_TCR2 = TCR2_BCD | 15u;               /* DIV=15, same both times */
            SAI_TCR5 = ((uint32_t)31 << 16);         /* 32-bit words            */
            SAI_RCR5 = ((uint32_t)31 << 16);
            for (i = 0; i < FIFO_DEPTH; i++) {
                SAI_TDR0 = 0x7000u + i;
            }
            t0 = SYST_CVR;
            SAI_TCSR = CSR_EN;
            for (d = 0; d < 200000000 && TFR_COUNT(SAI_TFR0) != 0; d++) {
            }
            t1 = SYST_CVR;
            SAI_TCSR = 0;
            if (sel) {
                drain_pll0 = (t0 - t1) & SYST_MASK;
            } else {
                drain_frohf = (t0 - t1) & SYST_MASK;
            }
        }

        r100 = drain_pll0 ? (drain_frohf * 100u) / drain_pll0 : 0;
        puts_("  MCLK=FRO_HF drain "); putdec(drain_frohf);
        puts_(";  MCLK=PLL0 drain "); putdec(drain_pll0);
        puts_(";  ratio x100 "); putdec(r100); puts_(" (expect 312 = 150/48)\r\n");
        /* 3.125x within a generous band (the polling loop's own overhead is the only slack). */
        ok &= (r100 > 297u && r100 < 328u);
    }

    puts_(ok ? "SAI PASS\r\n" : "SAI FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
