/*
 * MCXN947 FlexSPI external-NOR round-trip test (storage-write-verified, on the
 * FlexSPI-NOR medium).
 *
 * Drives the FlexSPI exactly as a real driver does — program the LUT, then issue
 * WREN / sector-erase / page-program / read through the IP command path — and
 * checks the data comes back byte-exact, both through the IP RX FIFO and through
 * the AHB (XIP) window, which is the same array.
 *
 * It also pins the silent-wrongs the bring-up model had, where the AHB NOR
 * window was memory_region_init_ram():
 *
 *   NEG1  a plain CPU store into the AHB/XIP window must do NOTHING.  A NOR is
 *         programmed only by an erase + page-program command sequence; backing
 *         the window with RAM let firmware scribble at XIP addresses and appear
 *         to work, and kept the entire IP command path an untested stub.
 *   NEG2  the IP path must really reach a NOR: the JEDEC ID must read back as
 *         the board's W25Q64 (EF 40 17), not zeros.
 *   NEG3  programming without an intervening erase must only CLEAR bits (NOR
 *         cannot set a 0 back to 1), so the result is old & new — corrupt,
 *         exactly as on silicon, rather than a clean overwrite.
 *
 * Prints "FLEXSPI-NOR PASS" only if every check holds.
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

#define FSPI 0x400C8000u
#define FSPI_INTR     (*(volatile uint32_t*)(FSPI+0x14))
#define FSPI_IPCR0    (*(volatile uint32_t*)(FSPI+0xA0))
#define FSPI_IPCR1    (*(volatile uint32_t*)(FSPI+0xA4))
#define FSPI_IPCMD    (*(volatile uint32_t*)(FSPI+0xB0))
#define FSPI_IPRXFCR  (*(volatile uint32_t*)(FSPI+0xB8))
#define FSPI_IPTXFCR  (*(volatile uint32_t*)(FSPI+0xBC))
#define FSPI_IPRXFSTS (*(volatile uint32_t*)(FSPI+0xF0))
#define FSPI_RFDR(i)  (*(volatile uint32_t*)(FSPI+0x100+(i)*4))
#define FSPI_TFDR(i)  (*(volatile uint32_t*)(FSPI+0x180+(i)*4))
#define FSPI_LUT(i)   (*(volatile uint32_t*)(FSPI+0x200+(i)*4))

#define IPRXFSTS_FILL(v) ((v) & 0xFF)

#define INTR_IPCMDDONE (1u << 0)
#define INTR_IPCMDERR  (1u << 3)
#define IPCMD_TRG      (1u << 0)
#define FCR_CLR        (1u << 0)

/* LUT instruction opcodes (SDK fsl_flexspi.h). */
#define OP_STOP   0x00
#define OP_CMD    0x01
#define OP_RADDR  0x02
#define OP_WRITE  0x08
#define OP_READ   0x09
#define OP_DUMMY  0x0C
#define PAD1      0

#define LUT_SEQ(c0, p0, o0, c1, p1, o1) \
    ((uint32_t)(o0) | ((uint32_t)(p0) << 8) | ((uint32_t)(c0) << 10) | \
     ((uint32_t)(o1) << 16) | ((uint32_t)(p1) << 24) | ((uint32_t)(c1) << 26))

/* Sequence slots (each is 4 LUT registers). */
#define SEQ_READ 0
#define SEQ_WREN 1
#define SEQ_SE   2
#define SEQ_PP   3
#define SEQ_RDID 4

/* The AHB (XIP) view of the NOR, secure aperture. */
#define NOR_AHB  0x90000000u
/* A test sector 1 MiB into the 8 MiB part, well clear of any XIP code. */
#define TEST_OFF 0x00100000u
#define TEST_AHB (NOR_AHB + TEST_OFF)

static void ip_run(uint32_t seq, uint32_t addr, uint32_t len)
{
    FSPI_IPRXFCR = FCR_CLR;
    FSPI_IPTXFCR = FCR_CLR;
    FSPI_INTR = INTR_IPCMDDONE | INTR_IPCMDERR;   /* W1C */
    FSPI_IPCR0 = addr;
    FSPI_IPCR1 = len | (seq << 16);
    FSPI_IPCMD = IPCMD_TRG;
}

static int ip_wait(void)
{
    while (!(FSPI_INTR & INTR_IPCMDDONE)) {
    }
    return !(FSPI_INTR & INTR_IPCMDERR);
}

/*
 * Drain a read the way the stock MCUXpresso driver does.  fsl_flexspi's
 * FLEXSPI_ReadBlocking() takes one of two paths, and a small read (below the RX
 * watermark) spins on the FIFO fill level:
 *
 *     while (size > ((IPRXFSTS & FILL_MASK) >> FILL_SHIFT) * 8U)
 *
 * FILL counts 64-bit entries, so it has to ROUND UP — a 3-byte JEDEC ID lives in
 * one partially-filled entry.  A model that rounds down reports FILL = 0 and the
 * real driver hangs here forever.  Polling it exactly as the driver does is the
 * point: reading RFDR blindly would pass against a model that can't run the
 * actual SDK.
 */
static int ip_drain(uint32_t *dst, uint32_t nwords)
{
    uint32_t size = nwords * 4;
    uint32_t spins = 0;

    while (size > IPRXFSTS_FILL(FSPI_IPRXFSTS) * 8u) {
        if (++spins > 100000u) {
            return 0;          /* the driver would have hung here */
        }
    }
    for (uint32_t i = 0; i < nwords; i++) {
        dst[i] = FSPI_RFDR(i);
    }
    return 1;
}

static int nor_wren(void)
{
    ip_run(SEQ_WREN, 0, 0);
    return ip_wait();
}

void cpu0_main(void)
{
    volatile uint32_t *nor = (volatile uint32_t *)TEST_AHB;
    static const uint32_t pat[4] =
        { 0xA5A5F00Fu, 0x0F0F0F0Fu, 0xDEADBEEFu, 0x12345678u };
    /* Asks for 1-bits where pat has 0-bits: a NOR cannot set them back. */
    static const uint32_t pat2[4] =
        { 0xFFFFFFFFu, 0xF0F0F0F0u, 0xFFFFFFFFu, 0xFFFFFFFFu };
    uint32_t id, back[4];
    int ok = 1;

    LP_CTRL = (1u << 19);
    puts_("FLEXSPI-NOR test\r\n");

    /* --- program the LUT, as a real driver does -------------------------- */
    FSPI_LUT(SEQ_READ * 4) = LUT_SEQ(OP_CMD, PAD1, 0x03, OP_RADDR, PAD1, 24);
    FSPI_LUT(SEQ_READ * 4 + 1) = LUT_SEQ(OP_READ, PAD1, 0x04, OP_STOP, PAD1, 0);

    FSPI_LUT(SEQ_WREN * 4) = LUT_SEQ(OP_CMD, PAD1, 0x06, OP_STOP, PAD1, 0);

    FSPI_LUT(SEQ_SE * 4) = LUT_SEQ(OP_CMD, PAD1, 0x20, OP_RADDR, PAD1, 24);
    FSPI_LUT(SEQ_SE * 4 + 1) = LUT_SEQ(OP_STOP, PAD1, 0, OP_STOP, PAD1, 0);

    FSPI_LUT(SEQ_PP * 4) = LUT_SEQ(OP_CMD, PAD1, 0x02, OP_RADDR, PAD1, 24);
    FSPI_LUT(SEQ_PP * 4 + 1) = LUT_SEQ(OP_WRITE, PAD1, 0x04, OP_STOP, PAD1, 0);

    FSPI_LUT(SEQ_RDID * 4) = LUT_SEQ(OP_CMD, PAD1, 0x9F, OP_READ, PAD1, 0x04);

    /* --- NEG2: the IP path must reach a NOR, drained as the SDK drains it -- */
    ip_run(SEQ_RDID, 0, 3);
    ok &= ip_wait();
    /* A 3-byte read is the small-read path: the driver spins on IPRXFSTS[FILL],
     * which must round up to 1 entry.  If it rounds down, this never returns. */
    ok &= ip_drain(&id, 1);
    ok &= ((id & 0xFFFFFFu) == 0x1740EFu);   /* EF 40 17, LE in the FIFO */

    /* --- erase the test sector ------------------------------------------- */
    ok &= nor_wren();
    ip_run(SEQ_SE, TEST_OFF, 0);
    ok &= ip_wait();

    /* An erased NOR reads all ones, through the AHB window. */
    for (int i = 0; i < 4; i++) {
        ok &= (nor[i] == 0xFFFFFFFFu);
    }

    /* --- NEG1: a CPU store into the XIP window must do NOTHING ----------- */
    nor[0] = 0x5A5A5A5Au;               /* external NOR is not RAM */
    ok &= (nor[0] == 0xFFFFFFFFu);      /* ...so it must not have landed */

    /* --- the round trip: page-program, then read back byte-exact --------- */
    ok &= nor_wren();
    ip_run(SEQ_PP, TEST_OFF, sizeof(pat));
    for (int i = 0; i < 4; i++) {
        FSPI_TFDR(i) = pat[i];          /* stream the program data */
    }
    ok &= ip_wait();

    /* read back through the IP path, polling the fill level like the driver... */
    ip_run(SEQ_READ, TEST_OFF, sizeof(pat));
    ok &= ip_wait();
    ok &= ip_drain(back, 4);
    for (int i = 0; i < 4; i++) {
        ok &= (back[i] == pat[i]);
    }
    /* ...and through the AHB/XIP window: the same array, so XIP sees it too. */
    for (int i = 0; i < 4; i++) {
        ok &= (nor[i] == pat[i]);
    }

    /* --- NEG3: programming without an erase can only CLEAR bits ---------- */
    ok &= nor_wren();
    ip_run(SEQ_PP, TEST_OFF, sizeof(pat2));
    for (int i = 0; i < 4; i++) {
        FSPI_TFDR(i) = pat2[i];
    }
    ok &= ip_wait();
    for (int i = 0; i < 4; i++) {
        ok &= (nor[i] == (pat[i] & pat2[i]));   /* NOR cannot set a 0 back to 1 */
    }

    /* --- erase restores 0xFF --------------------------------------------- */
    ok &= nor_wren();
    ip_run(SEQ_SE, TEST_OFF, 0);
    ok &= ip_wait();
    for (int i = 0; i < 4; i++) {
        ok &= (nor[i] == 0xFFFFFFFFu);
    }

    puts_(ok ? "FLEXSPI-NOR PASS\r\n" : "FLEXSPI-NOR FAIL\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
