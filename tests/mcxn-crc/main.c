/*
 * MCXN947 CRC engine test — checked against INDEPENDENT golden values.
 *
 * The CRC is a compute engine, and a compute engine can only be validated
 * against a reference answer that did not come from the model.  So this test
 * uses the published CRC check vectors: the standard value of each algorithm
 * over the string "123456789".  Nothing here is derived from our implementation.
 *
 *   CRC-16/CCITT-FALSE  poly 0x1021,      seed 0xFFFF,      no reflect, no xorout
 *                       -> 0x29B1
 *   CRC-32/MPEG-2       poly 0x04C11DB7,  seed 0xFFFFFFFF,  no reflect, no xorout
 *                       -> 0x0376E6E7
 *   CRC-32/IEEE         poly 0x04C11DB7,  seed 0xFFFFFFFF,  reflect in+out,
 *                       xorout 0xFFFFFFFF  -> 0xCBF43926
 *
 * THREE configurations, not one.  A single "correct" stamp proves the engine is
 * right at ONE shape and buys the trust that stops you looking at the others —
 * which is exactly how a shape-dependent defect survives a green suite.  Here the
 * three vectors exercise different widths, different polynomials, and all of the
 * TOT / TOTR / FXOR transpose paths.
 *
 * The message is 9 bytes — deliberately NOT a multiple of four — because the
 * stock SDK's CRC_WriteData() feeds the engine byte-wise (fsl_crc.c writes
 * ACCESS8BIT.DATALL) to align an unaligned buffer and to finish an odd-length
 * one.  A model whose DATA window is 4-byte-only rejects the real driver on any
 * message of odd length; feeding "123456789" as 4 + 4 + 1 exercises that.
 *
 * Prints "CRC PASS" only if all three goldens match.
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
static void puthex8(uint32_t v)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        putc_(h[(v >> (i * 4)) & 0xF]);
    }
}

#define CRC 0x400CB000u
#define CRC_DATA32 (*(volatile uint32_t *)(CRC + 0x0))
#define CRC_DATA8  (*(volatile uint8_t  *)(CRC + 0x0))
#define CRC_GPOLY  (*(volatile uint32_t *)(CRC + 0x4))
#define CRC_CTRL   (*(volatile uint32_t *)(CRC + 0x8))

#define CTRL_TCRC   (1u << 24)   /* 0 = 16-bit, 1 = 32-bit */
#define CTRL_WAS    (1u << 25)   /* next DATA write is the seed */
#define CTRL_FXOR   (1u << 26)   /* complement the result on read */
#define CTRL_TOTR(v) ((uint32_t)(v) << 28)   /* transpose on read  */
#define CTRL_TOT(v)  ((uint32_t)(v) << 30)   /* transpose on write */

/*
 * Run the check message "123456789" through the engine.
 * Fed as 4 + 4 + 1 bytes: the trailing byte write is what the SDK does, and what
 * a 4-byte-only DATA window would reject.
 */
static uint32_t crc_check_vector(uint32_t ctrl, uint32_t poly, uint32_t seed)
{
    CRC_GPOLY = poly;
    CRC_CTRL = ctrl | CTRL_WAS;      /* arm the seed load */
    CRC_DATA32 = seed;
    CRC_CTRL = ctrl;                 /* WAS off: subsequent writes are data */

    CRC_DATA32 = 0x31323334u;        /* "1234" */
    CRC_DATA32 = 0x35363738u;        /* "5678" */
    CRC_DATA8  = 0x39u;              /* "9"  <- byte write; the SDK does this */

    return CRC_DATA32;
}

void cpu0_main(void)
{
    uint32_t got;
    int ok = 1;

    LP_CTRL = (1u << 19);
    puts_("CRC test\r\n");

    /* --- CRC-16/CCITT-FALSE: golden 0x29B1 ------------------------------- */
    got = crc_check_vector(CTRL_TOT(0) | CTRL_TOTR(0), 0x1021u, 0xFFFFu) & 0xFFFFu;
    puts_("  crc16/ccitt-false = 0x"); puthex8(got); puts_("  want 0x000029b1\r\n");
    ok &= (got == 0x29B1u);

    /* --- CRC-32/MPEG-2: golden 0x0376E6E7 -------------------------------- */
    got = crc_check_vector(CTRL_TCRC | CTRL_TOT(0) | CTRL_TOTR(0),
                           0x04C11DB7u, 0xFFFFFFFFu);
    puts_("  crc32/mpeg-2      = 0x"); puthex8(got); puts_("  want 0x0376e6e7\r\n");
    ok &= (got == 0x0376E6E7u);

    /* --- CRC-32/IEEE: reflect in+out, final XOR.  golden 0xCBF43926 ------- */
    got = crc_check_vector(CTRL_TCRC | CTRL_TOT(1) | CTRL_TOTR(2) | CTRL_FXOR,
                           0x04C11DB7u, 0xFFFFFFFFu);
    puts_("  crc32/ieee        = 0x"); puthex8(got); puts_("  want 0xcbf43926\r\n");
    ok &= (got == 0xCBF43926u);

    puts_(ok ? "CRC PASS\r\n" : "CRC FAIL\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
