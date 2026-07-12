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

    puts_(ok ? "SAI PASS\r\n" : "SAI FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
