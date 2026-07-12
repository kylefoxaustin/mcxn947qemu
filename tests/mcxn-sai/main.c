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

    /* 32-bit words at the fastest bit clock: a short word period keeps it brisk. */
    SAI_TCR2 = 0;                 /* DIV = 0                              */
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
            SAI_TCR2 = divs[k];                 /* bit-clock divider     */
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

    puts_(ok ? "SAI PASS\r\n" : "SAI FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
