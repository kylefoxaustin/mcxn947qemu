/*
 * MCXN947 SINC sigma-delta filter test.
 *
 * Drives the SINC through its RM-documented *register-fed* bitstream path
 * (CnCFR[IBFMT] = 10b, "PM": each CnMPDATA write feeds 16 modulator bits to the
 * CIC — RM rev 7 §47.3.2.3.7), and asserts the filter produces the arithmetically
 * correct decimated result rather than a plausible-looking constant.
 *
 * Configured with ORD = 1 and OSR = 16, the CIC
 *
 *     H(z) = ( (1 - z^-OSR) / (1 - z^-1) ) ^ ORD
 *
 * reduces to "sum the 16 bits of the decimation window", so each 16-bit MPDATA
 * write yields exactly its popcount.  That is hand-checkable and independent of
 * the bit order the hardware shifts in, so the test asserts the maths, not an
 * implementation detail.
 *
 * It also pins the two things the old register-file model got wrong, both of
 * which are silent-wrong-answers on real silicon:
 *
 *   NEG1  SR[MCLKRDY0] must be 0 before MCR[MEN] and 1 after.  The old model
 *         hardwired SR = 0x1F00, so MCLKRDY was stuck at 0 and the stock NXP
 *         SINC_Init() spun forever.
 *   NEG2  SR[FIFOEMPTY0] must be 1 when nothing has been converted.  The old
 *         model reported "not empty" forever, so firmware read an endless run
 *         of zeros it could not distinguish from real samples.
 *
 * Prints "SINC PASS" only if every check holds.
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

#define SINC 0x40108000u
#define SINC_MCR   (*(volatile uint32_t*)(SINC+0x08))
#define SINC_NIS   (*(volatile uint32_t*)(SINC+0x18))
#define SINC_SR    (*(volatile uint32_t*)(SINC+0x24))
/* Channel 0: base 0x38, step 0x30 */
#define C0_CCR     (*(volatile uint32_t*)(SINC+0x38))
#define C0_CDR     (*(volatile uint32_t*)(SINC+0x3C))
#define C0_CCFR    (*(volatile uint32_t*)(SINC+0x40))
#define C0_CBIAS   (*(volatile uint32_t*)(SINC+0x48))
#define C0_CRDATA  (*(volatile uint32_t*)(SINC+0x54))
#define C0_CMPDATA (*(volatile uint32_t*)(SINC+0x58))
#define C0_CSR     (*(volatile uint32_t*)(SINC+0x60))

/* MCR */
#define MCR_STRIG0   (1u << 0)
#define MCR_MEN      (1u << 15)
/* SR */
#define SR_CHRDY0    (1u << 8)
#define SR_FIFOEMPTY0 (1u << 16)
#define SR_MCLKRDY0  (1u << 24)
/* NIS */
#define NIS_COC0     (1u << 0)
/* CCR */
#define CCR_CHEN     (1u << 0)
#define CCR_PFEN     (1u << 1)
#define CCR_FIFOEN   (1u << 14)
/* CCFR */
#define CCFR_RDFMT   (1u << 6)          /* 1 = unsigned bitstream {0,1} */
#define CCFR_IBFMT_PM (2u << 16)
/* CSR */
#define CSR_FIFOAVIL 0x1Fu
#define CSR_PSRDY    (1u << 7)

/* popcount of the low 16 bits — the expected CIC output for ORD=1, OSR=16. */
static uint32_t pop16(uint32_t v)
{
    uint32_t n = 0;
    for (int i = 0; i < 16; i++) {
        n += (v >> i) & 1;
    }
    return n;
}

/*
 * Run one (ORD, OSR) shape with an all-ones bitstream and require the settled
 * output to equal the CIC's DC gain, OSR^ORD.  Returns 1 on match.
 *
 * Feeds SETTLE_WINDOWS decimation windows (comfortably more than ORD+1, so the
 * comb section has settled) and checks the LAST result.  16 bits arrive per PM
 * write, so a window needs OSR/16 writes when OSR > 16, and one write covers
 * 16/OSR windows when OSR < 16.
 */
#define SETTLE_WINDOWS 6

static int sweep_shape(uint32_t ord, uint32_t osr, uint32_t expect)
{
    uint32_t bits_needed = SETTLE_WINDOWS * osr;
    uint32_t words = (bits_needed + 15) / 16;
    uint32_t got = 0;
    uint32_t n;

    /* Reconfigure the channel for this shape. */
    C0_CCR   = 0;                          /* disable: resets the filter state */
    SINC_NIS = 0xFFFFFFFFu;                /* clear stale flags               */
    C0_CDR   = (osr - 1u) | (ord << 11) | (1u << 14);   /* PFOSR, PFORD, cont */
    C0_CCFR  = CCFR_RDFMT | CCFR_IBFMT_PM | (0u << 10);
    C0_CBIAS = 0;
    C0_CCR   = CCR_CHEN | CCR_PFEN | CCR_FIFOEN;

    SINC_MCR = MCR_MEN | MCR_STRIG0;       /* trigger */

    for (n = 0; n < words; n++) {
        while (!(C0_CSR & CSR_PSRDY)) {
        }
        C0_CMPDATA = 0xFFFFu;              /* all ones: DC input */
        /* Drain as we go so the 8-deep FIFO cannot overflow. */
        while (!(SINC_SR & SR_FIFOEMPTY0)) {
            got = C0_CRDATA >> 8;
        }
    }

    /* The settled output of a CIC fed a constant 1 is its DC gain, OSR^ORD. */
    return got == expect;
}

void cpu0_main(void)
{
    /* Three 16-bit modulator words -> three decimated results. */
    static const uint32_t stream[3] = { 0xFFFFu, 0x0000u, 0xAAAAu };
    int ok = 1;

    LP_CTRL = (1u << 19);
    puts_("SINC test\r\n");

    /* --- NEG1: the modulator clock is NOT ready before MCR[MEN] ---------- */
    ok &= !(SINC_SR & SR_MCLKRDY0);

    SINC_MCR = MCR_MEN;

    /* ...and IS ready after.  The stock SDK's SINC_Init() spins here. */
    ok &= !!(SINC_SR & SR_MCLKRDY0);

    /* --- configure channel 0: ORD = 1, OSR = 16, unsigned, PM input ------ */
    C0_CDR   = (16u - 1u)          /* PFOSR: OSR = PFOSR + 1 = 16 */
             | (1u << 11)          /* PFORD = 1  -> CIC order 1   */
             | (1u << 14);         /* PFCM  = 01 -> continuous    */
    C0_CCFR  = CCFR_RDFMT          /* unsigned bitstream {0,1}    */
             | CCFR_IBFMT_PM       /* bitstream comes from CMPDATA */
             | (0u << 10);         /* FIFOWMK = 0                 */
    C0_CBIAS = 0;
    C0_CCR   = CCR_CHEN | CCR_PFEN | CCR_FIFOEN;

    ok &= !!(SINC_SR & SR_CHRDY0);

    /* --- NEG2: nothing converted yet, so the FIFO must report EMPTY ------ */
    ok &= !!(SINC_SR & SR_FIFOEMPTY0);
    ok &= ((C0_CSR & CSR_FIFOAVIL) == 0);

    /* --- feed the bitstream --------------------------------------------- */
    SINC_MCR = MCR_MEN | MCR_STRIG0;      /* every PFCM mode needs a trigger */

    for (int i = 0; i < 3; i++) {
        ok &= !!(C0_CSR & CSR_PSRDY);     /* ready to accept the next word */
        C0_CMPDATA = stream[i];           /* 16 modulator bits -> the CIC  */
    }

    /* Three decimation windows completed -> three results queued. */
    ok &= !(SINC_SR & SR_FIFOEMPTY0);
    ok &= ((C0_CSR & CSR_FIFOAVIL) == 3);
    ok &= !!(SINC_NIS & NIS_COC0);

    /* --- the results must be the actual filter output -------------------- */
    for (int i = 0; i < 3; i++) {
        uint32_t got = C0_CRDATA >> 8;    /* RDATA lives in bits [31:8] */

        ok &= (got == pop16(stream[i]));  /* ORD=1, OSR=16 => popcount   */
    }

    /*
     * --- SHAPE SWEEP: one shape is not a golden ---------------------------
     *
     * The checks above validate the filter at ORD=1, OSR=16 and nowhere else.
     * A defect that is order- or rate-dependent would sail straight through
     * them, which is exactly how a shape-dependent bug survives a green suite.
     * (ollama_95_neutron measured NXP's shipped Neutron computing correctly at
     * one shape and writing garbage at another; rt1180emulator hit the same
     * class one commit after believing he had fixed it.)
     *
     * The golden here is closed-form and independent of this implementation: the
     * DC gain of a CIC is H(1) = OSR^ORD.  So an all-ones bitstream, once the
     * transient has settled, MUST decimate to exactly OSR^ORD at every order and
     * every oversampling ratio.  That number comes from the transfer function,
     * not from the model.
     */
    ok &= sweep_shape(1, 16, 16u);          /* 16^1 */
    ok &= sweep_shape(2, 16, 256u);         /* 16^2 */
    ok &= sweep_shape(3, 8,  512u);         /* 8^3  */
    ok &= sweep_shape(2, 32, 1024u);        /* 32^2 */
    ok &= sweep_shape(1, 4,  4u);           /* 4^1  */

    /* Drained: EMPTY again. */
    ok &= !!(SINC_SR & SR_FIFOEMPTY0);
    ok &= ((C0_CSR & CSR_FIFOAVIL) == 0);

    puts_(ok ? "SINC PASS\r\n" : "SINC FAIL\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
