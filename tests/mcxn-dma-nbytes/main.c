/*
 * MCXN947 eDMA — NBYTES that is NOT a multiple of the transfer size.
 *
 * ⭐ THE BUG THIS EXISTS FOR, AND HOW IT SURVIVED.
 *
 * The minor loop was `for (b = 0; b + step <= nbytes; b += step)`.  With
 * NBYTES = 6 and a 4-byte transfer size that moved FOUR BYTES AND DROPPED TWO --
 * no error, no flag, DONE set, INTMAJOR raised, and the guest told its transfer
 * had COMPLETED.  Silent data loss, in a DMA engine.
 *
 * EVERY eDMA TEST IN THIS TREE USED NBYTES = 4 WITH A 4-BYTE TRANSFER SIZE.
 * Perfectly round -- and at every round value the bug does not exist.
 *
 * Found by carrying ollama_95_neutron's rule one column to the right:
 *
 *     "ROUND NUMBERS ARE HOW BUGS SURVIVE -- a 2^n sweep sails straight past the
 *      broken values.  I wrote that rule AFTER the K bug found me, AND THEN SWEPT
 *      THE VERY NEXT AXIS WITH ROUND NUMBERS.  I learned the lesson ON the axis
 *      that taught it, and did not carry it ONE COLUMN TO THE RIGHT."
 *
 * Real eDMA validates the TCD BEFORE it moves anything: a NBYTES that is not a
 * multiple of the transfer size is a CONFIGURATION ERROR (CH_ES[NCE]) and the
 * channel REFUSES TO RUN.
 *
 * Checks, sweeping UGLY values:
 *   NBYTES = 4, 8, 16   (multiples of 4) -> transfer runs, data byte-exact
 *   NBYTES = 3, 5, 6, 7 (NOT multiples)  -> CH_ES[NCE] + [ERR], NO DONE,
 *                                           AND THE DESTINATION IS UNTOUCHED
 *
 * Prints "DMANB PASS" only if every case holds.
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

#define DMA0 0x40080000u
#define CH(n) (DMA0 + 0x1000u * ((n) + 1))
#define CH_CSR(n)    (*(volatile uint32_t *)(CH(n) + 0x00))
#define CH_ES(n)     (*(volatile uint32_t *)(CH(n) + 0x04))
#define TCD_SADDR(n) (*(volatile uint32_t *)(CH(n) + 0x20))
#define TCD_SOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x24))
#define TCD_ATTR(n)  (*(volatile uint16_t *)(CH(n) + 0x26))
#define TCD_NBYTES(n)(*(volatile uint32_t *)(CH(n) + 0x28))
#define TCD_SLAST(n) (*(volatile uint32_t *)(CH(n) + 0x2C))
#define TCD_DADDR(n) (*(volatile uint32_t *)(CH(n) + 0x30))
#define TCD_DOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x34))
#define TCD_CITER(n) (*(volatile uint16_t *)(CH(n) + 0x36))
#define TCD_DLAST(n) (*(volatile uint32_t *)(CH(n) + 0x38))
#define TCD_CSR(n)   (*(volatile uint16_t *)(CH(n) + 0x3C))
#define TCD_BITER(n) (*(volatile uint16_t *)(CH(n) + 0x3E))

#define CSR_DONE   (1u << 30)
#define TCD_START  (1u << 0)
#define ATTR_32BIT ((2u << 8) | 2u)   /* SSIZE = DSIZE = 2 -> 4 bytes */

#define ES_NCE (1u << 3)
#define ES_DOE (1u << 4)
#define ES_DAE (1u << 5)
#define ES_SOE (1u << 6)
#define ES_SAE (1u << 7)
#define ES_ERR (1u << 31)

#define CHAN 0

/* SAI0 TX FIFO — a REAL MMIO register, where the WRITE WIDTH actually matters.
 * One 32-bit write pushes ONE word; four byte writes push FOUR.  Memory cannot
 * tell those apart; a FIFO occupancy counter can. */
#define SAI0 0x40106000u
#define SAI_TCSR (*(volatile uint32_t *)(SAI0 + 0x08))
#define SAI_TDR0_ADDR            (SAI0 + 0x20)
#define SAI_TFR0 (*(volatile uint32_t *)(SAI0 + 0x40))
#define SAI_FR   (1u << 25)                     /* FIFO reset */
#define TFR_COUNT(v) (((v) >> 16) & 0xF)
#define NBUF 16

static volatile uint8_t src[NBUF];
static volatile uint8_t dst[NBUF];

/* Run one minor loop of `nbytes` with a 4-byte transfer size. */
static void arm_and_start(uint32_t nbytes)
{
    CH_ES(CHAN)      = 0xFFFFFFFFu;         /* W1C any stale error */
    CH_CSR(CHAN)     = 0;
    TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)src;
    TCD_SOFF(CHAN)   = 4;
    TCD_ATTR(CHAN)   = ATTR_32BIT;
    TCD_NBYTES(CHAN) = nbytes;
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)dst;
    TCD_DOFF(CHAN)   = 4;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = 1;
    TCD_BITER(CHAN)  = 1;
    TCD_CSR(CHAN)    = TCD_START;           /* software trigger */
}

void cpu0_main(void)
{
    /* UGLY values first — 3, 5, 6, 7 are exactly what a 2^n sweep skips. */
    static const uint8_t bad[4]  = { 3, 5, 6, 7 };
    static const uint8_t good[3] = { 4, 8, 16 };
    int ok = 1;
    int i, j;

    LP_CTRL = CTRL_TE;
    puts_("eDMA NBYTES test\r\n");

    for (i = 0; i < NBUF; i++) {
        src[i] = (uint8_t)(0xA0 + i);
    }

    /* --- NOT a multiple of the 4-byte transfer size: MUST REFUSE --------- */
    for (i = 0; i < 4; i++) {
        int this_ok = 1;

        for (j = 0; j < NBUF; j++) {
            dst[j] = 0xEE;                  /* poison: a partial move shows */
        }
        arm_and_start(bad[i]);

        this_ok &= !!(CH_ES(CHAN) & ES_NCE);     /* config error flagged     */
        this_ok &= !!(CH_ES(CHAN) & ES_ERR);
        this_ok &= !(CH_CSR(CHAN) & CSR_DONE);   /* NOT reported complete    */
        for (j = 0; j < NBUF; j++) {
            this_ok &= (dst[j] == 0xEE);         /* AND NOTHING WAS MOVED    */
        }

        puts_(this_ok ? "  ok   NBYTES=" : "  FAIL NBYTES=");
        putdec(bad[i]);
        puts_(" -> refused (NCE)\r\n");
        ok &= this_ok;
    }

    /* --- a proper multiple: MUST RUN, byte-exact ------------------------- */
    for (i = 0; i < 3; i++) {
        int this_ok = 1;
        uint32_t n = good[i];

        for (j = 0; j < NBUF; j++) {
            dst[j] = 0xEE;
        }
        arm_and_start(n);

        this_ok &= !(CH_ES(CHAN) & ES_ERR);      /* no error                 */
        this_ok &= !!(CH_CSR(CHAN) & CSR_DONE);  /* really completed         */
        for (j = 0; j < (int)n; j++) {
            this_ok &= (dst[j] == src[j]);       /* byte-exact               */
        }
        for (j = (int)n; j < NBUF; j++) {
            this_ok &= (dst[j] == 0xEE);         /* and NOT one byte further */
        }

        puts_(this_ok ? "  ok   NBYTES=" : "  FAIL NBYTES=");
        putdec(n);
        puts_(" -> moved byte-exact\r\n");
        ok &= this_ok;
    }

    /* --- the OTHER axes, which I had also only ever tested at ROUND values ---
     *
     * Every test in this tree used SOFF = 4 and an aligned buffer.  A misaligned
     * SADDR or an offset that is not a multiple of the transfer size is a
     * CONFIGURATION ERROR on real silicon (CH_ES[SAE]/[SOE]/[DAE]/[DOE]) -- the
     * channel refuses to run.  The model used to walk them happily and produce
     * garbage.  I DEFINED these four bits when I fixed NBYTES and IMPLEMENTED
     * NONE OF THEM: a dead error channel created while fixing a dead error
     * channel.  Sweeping the ugly values is the only thing that finds that.
     */
    {
        static const char  *const what[4] = { "SADDR misaligned",
                                              "DADDR misaligned",
                                              "SOFF not x4",
                                              "DOFF not x4" };
        static const uint8_t  sa[4]  = { 1, 0, 0, 0 };   /* SADDR byte offset */
        static const uint8_t  da[4]  = { 0, 2, 0, 0 };   /* DADDR byte offset */
        static const int8_t   so[4]  = { 4, 4, 3, 4 };   /* SOFF              */
        static const int8_t   dof[4] = { 4, 4, 4, 6 };   /* DOFF              */
        static const uint32_t exp[4] = { ES_SAE, ES_DAE, ES_SOE, ES_DOE };
        int k;

        for (k = 0; k < 4; k++) {
            int this_ok = 1;

            for (j = 0; j < NBUF; j++) {
                dst[j] = 0xEE;
            }
            CH_ES(CHAN)      = 0xFFFFFFFFu;
            CH_CSR(CHAN)     = 0;
            TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)src + sa[k];
            TCD_SOFF(CHAN)   = (uint16_t)(int16_t)so[k];
            TCD_ATTR(CHAN)   = ATTR_32BIT;
            TCD_NBYTES(CHAN) = 4;
            TCD_SLAST(CHAN)  = 0;
            TCD_DADDR(CHAN)  = (uint32_t)(uintptr_t)dst + da[k];
            TCD_DOFF(CHAN)   = (uint16_t)(int16_t)dof[k];
            TCD_DLAST(CHAN)  = 0;
            TCD_CITER(CHAN)  = 1;
            TCD_BITER(CHAN)  = 1;
            TCD_CSR(CHAN)    = TCD_START;

            this_ok &= !!(CH_ES(CHAN) & exp[k]);           /* the RIGHT bit    */
            this_ok &= !!(CH_ES(CHAN) & ES_ERR);
            this_ok &= !(CH_CSR(CHAN) & CSR_DONE);         /* not "complete"   */
            for (j = 0; j < NBUF; j++) {
                this_ok &= (dst[j] == 0xEE);               /* nothing moved    */
            }

            puts_(this_ok ? "  ok   " : "  FAIL ");
            puts_(what[k]);
            puts_(" -> refused\r\n");
            ok &= this_ok;
        }
    }

    /* --- THE JOINT AXIS: SSIZE x DSIZE, which was NEVER VARIED TOGETHER -------
     *
     * SSIZE and DSIZE are SEPARATE fields and the engine used SSIZE for BOTH the
     * read and the write -- so SSIZE=1/DSIZE=4 issued FOUR BYTE WRITES where the
     * TCD asked for ONE 32-BIT WRITE.  To a MEMORY destination that is the same
     * bytes and the bug is INVISIBLE.  To an MMIO REGISTER it is a different
     * transaction entirely.
     *
     * Every ATTR in this tree set SSIZE == DSIZE.  Each field was swept ALONE and
     * each was individually correct, so the per-axis tests did not merely MISS this
     * -- THEY CERTIFIED IT.  (ollama_95_neutron: "an independent per-axis whitelist
     * does not merely miss the bug -- IT CERTIFIES IT.")
     *
     * The destination here is the SAI TX FIFO -- a real MMIO register with real
     * FIFO semantics -- because that is where the width actually matters.  One
     * 32-bit write pushes ONE word; four byte writes push FOUR.  TFR0's occupancy
     * counter tells them apart, and memory never could.
     */
    {
        static const uint8_t ss[3] = { 2, 0, 1 };   /* SSIZE: 4B, 1B, 2B      */
        static const uint8_t ds[3] = { 2, 2, 2 };   /* DSIZE: always 4B (MMIO) */
        int k;

        for (k = 0; k < 3; k++) {
            int this_ok = 1;
            uint32_t before, after;

            SAI_TCSR = SAI_FR;                       /* flush the TX FIFO      */
            before = TFR_COUNT(SAI_TFR0);
            this_ok &= (before == 0);

            CH_ES(CHAN)      = 0xFFFFFFFFu;
            CH_CSR(CHAN)     = 0;
            TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)src;
            TCD_SOFF(CHAN)   = (uint16_t)(1u << ss[k]);   /* walk the source   */
            TCD_ATTR(CHAN)   = (uint16_t)((ss[k] << 8) | ds[k]);
            TCD_NBYTES(CHAN) = 4;                    /* ONE 32-bit destination */
            TCD_SLAST(CHAN)  = 0;
            TCD_DADDR(CHAN)  = SAI_TDR0_ADDR;        /* an MMIO FIFO port      */
            TCD_DOFF(CHAN)   = 0;                    /* the FIFO does not move */
            TCD_DLAST(CHAN)  = 0;
            TCD_CITER(CHAN)  = 1;
            TCD_BITER(CHAN)  = 1;
            TCD_CSR(CHAN)    = TCD_START;

            after = TFR_COUNT(SAI_TFR0);

            /* NBYTES=4 into a 4-byte destination must be exactly ONE FIFO push,
             * whatever width the SOURCE was read in. */
            this_ok &= !(CH_ES(CHAN) & ES_ERR);
            this_ok &= (after == 1);

            puts_(this_ok ? "  ok   " : "  FAIL ");
            puts_("SSIZE="); putdec(1u << ss[k]);
            puts_("B DSIZE="); putdec(1u << ds[k]);
            puts_("B -> FIFO pushes="); putdec(after);
            puts_(" (want 1)\r\n");
            ok &= this_ok;
        }
        SAI_TCSR = 0;
    }

    puts_(ok ? "DMANB PASS\r\n" : "DMANB FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
