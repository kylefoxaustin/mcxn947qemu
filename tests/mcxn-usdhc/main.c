/*
 * MCXN947 uSDHC data-path test — blocks really move, to and from a REAL card.
 *
 * The old model CONJURED ITS OWN CARD, and the capability table claimed "uSDHC
 * ADMA block data" for it.  There was no ADMA and no block data: no DMA, no
 * descriptor walk, no storage — the model could not move one byte.  The card's
 * CMD8/CMD3/ACMD41/CMD2 responses were invented in CMD_RSP0..3, and CINST was
 * hardwired so firmware "detected" a card that was not there.
 *
 * AND THE OLD TEST PASSED THE MUTATION AUDIT ANYWAY.  It did read values back
 * and compare them, so it looked properly guarded — mutate CMD_RSP0 and it duly
 * failed.  But CMD8 only "passed" because the model ECHOED THIS TEST'S OWN
 * ARGUMENT back (0x1AA in, 0x1AA out), and CMD3 only "passed" because the test
 * had been told the model's hardcoded RCA.  Both sides of the comparison came
 * out of the same fiction: THE MODEL WAS ITS OWN ORACLE.  Mutation testing
 * proves a test is COUPLED to the model, not that it checks anything REAL —
 * against a conjured peer it is blind by construction.  The golden has to live
 * somewhere the model cannot reach.
 *
 * So it does now.  The uSDHC model supplies only the BUS; the harness attaches
 * a genuine QEMU sd-card backed by a file on the host:
 *
 *     -device sd-card,drive=sd0 -drive if=none,id=sd0,format=raw,file=card.img
 *
 * The golden is that file.  Nothing in the uSDHC model can produce its contents
 * by accident, and the harness re-checks the bytes ON THE HOST after QEMU exits
 * — so the write really left the emulator.
 *
 * Checks:
 *   1  a real card is present (CINST from the bus) and really enumerates:
 *      CMD0/CMD8/ACMD41/CMD2/CMD3 over the real bus, and the RCA comes from the
 *      CARD, not from us.
 *   2  read block 0, which the HOST filled before boot: bytes the guest never
 *      supplied, so an echoing model cannot fake them.
 *   3  write block 1 through a real ADMA2 descriptor and read it back through
 *      another — byte-exact.  The harness then verifies those bytes landed in
 *      the image file on the host.
 *
 * NEG1 (harness): with NO card attached, a command must TIME OUT
 *      (INT_STATUS[CTOE]) instead of returning a plausible response, and CINST
 *      must read 0.  A host that always answers is lying about an empty slot.
 *
 * Prints "USDHC PASS" when every check holds, or "USDHC NOCARD" when it
 * correctly saw an empty slot time out.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
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

#define USDHC 0x40109000u
#define BLK_ATT       (*(volatile uint32_t *)(USDHC + 0x04))
#define CMD_ARG       (*(volatile uint32_t *)(USDHC + 0x08))
#define CMD_XFR_TYP   (*(volatile uint32_t *)(USDHC + 0x0C))
#define CMD_RSP0      (*(volatile uint32_t *)(USDHC + 0x10))
#define PRES_STATE    (*(volatile uint32_t *)(USDHC + 0x24))
#define PROT_CTRL     (*(volatile uint32_t *)(USDHC + 0x28))
#define INT_STATUS    (*(volatile uint32_t *)(USDHC + 0x30))
#define MIX_CTRL      (*(volatile uint32_t *)(USDHC + 0x48))
#define ADMA_SYS_ADDR (*(volatile uint32_t *)(USDHC + 0x58))

#define INT_CC     (1u << 0)
#define INT_TC     (1u << 1)
#define INT_CTOE   (1u << 16)
#define PRES_CINST (1u << 16)

#define MIX_DMAEN  (1u << 0)
#define MIX_BCEN   (1u << 1)
#define MIX_DTDSEL (1u << 4)     /* 1 = card -> host */

#define DMASEL_ADMA2 (2u << 8)   /* PROT_CTRL[DMASEL] */

#define CMD(i) ((uint32_t)(i) << 24)
#define DPSEL  (1u << 21)

/* ADMA2 descriptor attributes. */
#define A_VALID (1u << 0)
#define A_END   (1u << 1)
#define A_TRAN  (2u << 4)

#define BLKSZ 512

static volatile uint8_t  wbuf[BLKSZ];
static volatile uint8_t  rbuf[BLKSZ];
static volatile uint64_t desc[2];

/* Block 0 of the image is pre-filled BY THE HARNESS with this pattern.  The
 * guest never writes it, so an echoing model cannot produce it. */
static uint8_t host_byte(int i)
{
    return (uint8_t)(0xC3u ^ (uint8_t)(i * 7));
}

/* Our own pattern, written to block 1 and read back. */
static uint8_t our_byte(int i)
{
    return (uint8_t)(0xA5u + (uint8_t)(i * 31));
}

/* Issue a command.  Returns 0 if it TIMED OUT (empty slot / refused). */
static int cmd(uint32_t xfr, uint32_t arg)
{
    INT_STATUS = 0xFFFFFFFFu;           /* W1C anything stale */
    CMD_ARG = arg;
    CMD_XFR_TYP = xfr;
    if (INT_STATUS & INT_CTOE) {
        return 0;
    }
    return (INT_STATUS & INT_CC) != 0;
}

/* Arm a one-descriptor ADMA2 transfer of a single block. */
static void adma_one(volatile void *buf)
{
    desc[0] = (uint64_t)(A_VALID | A_END | A_TRAN)
            | ((uint64_t)BLKSZ << 16)
            | ((uint64_t)(uint32_t)(uintptr_t)buf << 32);
    ADMA_SYS_ADDR = (uint32_t)(uintptr_t)&desc[0];
    PROT_CTRL = (PROT_CTRL & ~(3u << 8)) | DMASEL_ADMA2;
    BLK_ATT = (1u << 16) | BLKSZ;       /* one block of BLKSZ bytes */
}

void cpu0_main(void)
{
    uint32_t rca;
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE;
    puts_("USDHC test\r\n");

    /* --- NEG1: an EMPTY SLOT must not report a card, and must time out ----- */
    if (!(PRES_STATE & PRES_CINST)) {
        /* No card attached.  A command must TIME OUT rather than be answered
         * on the absent card's behalf. */
        int answered = cmd(CMD(8), 0x1AA);

        puts_((!answered && (INT_STATUS & INT_CTOE)) ? "USDHC NOCARD\r\n"
                                                     : "USDHC FAIL\r\n");
        for (;;) {
        }
    }

    /* --- 1: a real card enumerates over a real bus ------------------------- */
    (void)cmd(CMD(0), 0);                  /* GO_IDLE_STATE (no response)    */
    ok &= cmd(CMD(8), 0x1AA);              /* SEND_IF_COND                   */

    for (i = 0; i < 100; i++) {            /* ACMD41 until the card is ready */
        cmd(CMD(55), 0);                   /* APP_CMD                        */
        cmd(CMD(41), 0x40FF8000u);         /* SD_SEND_OP_COND (HCS)          */
        if (CMD_RSP0 & 0x80000000u) {      /* OCR power-up-done              */
            break;
        }
    }
    ok &= (i < 100);

    ok &= cmd(CMD(2), 0);                  /* ALL_SEND_CID (R2, 128-bit)     */
    ok &= cmd(CMD(3), 0);                  /* SEND_RELATIVE_ADDR             */
    rca = CMD_RSP0 >> 16;                  /* the CARD chose this, not us    */
    ok &= cmd(CMD(7), rca << 16);          /* SELECT_CARD                    */
    ok &= cmd(CMD(16), BLKSZ);             /* SET_BLOCKLEN                   */

    /* --- 2: read block 0 — filled by the HOST before boot ------------------ */
    for (i = 0; i < BLKSZ; i++) {
        rbuf[i] = 0xEE;                    /* poison: a stub leaves this      */
    }
    adma_one(rbuf);
    MIX_CTRL = MIX_DMAEN | MIX_BCEN | MIX_DTDSEL;
    ok &= cmd(CMD(17) | DPSEL, 0);         /* READ_SINGLE_BLOCK @ 0           */
    ok &= !!(INT_STATUS & INT_TC);
    for (i = 0; i < BLKSZ; i++) {
        ok &= (rbuf[i] == host_byte(i));   /* bytes the guest never supplied  */
    }

    /* --- 3: write block 1 via ADMA2, then read it back --------------------- */
    for (i = 0; i < BLKSZ; i++) {
        wbuf[i] = our_byte(i);
    }
    adma_one(wbuf);
    MIX_CTRL = MIX_DMAEN | MIX_BCEN;       /* DTDSEL clear = host -> card     */
    ok &= cmd(CMD(24) | DPSEL, BLKSZ);     /* WRITE_BLOCK @ byte 512          */
    ok &= !!(INT_STATUS & INT_TC);

    for (i = 0; i < BLKSZ; i++) {
        rbuf[i] = 0xEE;
    }
    adma_one(rbuf);
    MIX_CTRL = MIX_DMAEN | MIX_BCEN | MIX_DTDSEL;
    ok &= cmd(CMD(17) | DPSEL, BLKSZ);     /* READ_SINGLE_BLOCK @ 512         */
    ok &= !!(INT_STATUS & INT_TC);
    for (i = 0; i < BLKSZ; i++) {
        ok &= (rbuf[i] == our_byte(i));    /* byte-exact round trip           */
    }

    puts_(ok ? "USDHC PASS\r\n" : "USDHC FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
