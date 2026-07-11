/*
 * MCXN947 FMU flash program/erase round-trip test.
 *
 * This is the "storage-write-verified" rung: erase a sector, program it, read
 * the data back and check it is byte-exact.  It drives the FMU exactly as the
 * RM specifies (rev 7 §8.3.2.11/12/14) — the PEWEN/PERDY handshake, with the
 * target address and data supplied as CPU stores into the flash address space
 * while the controller has writes enabled.
 *
 * It also asserts the three things a register-file model gets wrong, each of
 * which is a silent-wrong-answer on real silicon:
 *
 *   NEG1  a store to flash outside a program/erase window does NOTHING
 *         (flash is not RAM)
 *   NEG2  programming a phrase twice without an intervening erase FAILS,
 *         and can only clear bits (1 -> 0), never set them
 *   NEG3  verify-erased (Read 1s Phrase) reports FAIL on a programmed phrase
 *
 * Prints "FMU PASS" only if every check holds.
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

#define FMU 0x40043000u
#define FMU_FSTAT   (*(volatile uint32_t*)(FMU+0x00))
#define FMU_FCCOB0  (*(volatile uint32_t*)(FMU+0x10))
#define FMU_FCCOB2  (*(volatile uint32_t*)(FMU+0x18))

/* FSTAT bits (CMSIS FMU_FSTAT_*) */
#define FAIL    (1u << 0)
#define CMDABT  (1u << 2)
#define PVIOL   (1u << 4)
#define ACCERR  (1u << 5)
#define CCIF    (1u << 7)
#define PEWEN   (3u << 24)
#define PERDY   (1u << 31)
#define ERRORS  (FAIL | CMDABT | PVIOL | ACCERR)

/* FCMD codes */
#define ERSSCR  0x42
#define PGMPHR  0x24
#define PGMPG   0x23
#define RD1SCR  0x02
#define RD1PHR  0x04

#define PHRASE_WORDS 4     /* 16 B: smallest programmable unit  */
#define PAGE_WORDS   32    /* 128 B: largest programmable unit  */

/* A sector well past this test's own code, which runs XIP from flash. */
#define SECTOR   0x10010000u
#define PHRASE_A (SECTOR + 0x00)
#define PHRASE_B (SECTOR + 0x10)
#define PAGE_C   (SECTOR + 0x80)   /* 128-byte aligned */

static void wait_ccif(void)  { while (!(FMU_FSTAT & CCIF))  {} }
static void wait_pewen(void) { while (!(FMU_FSTAT & PEWEN)) {} }
static void wait_perdy(void) { while (!(FMU_FSTAT & PERDY)) {} }

/* Stage a command and launch it.  Errors must be cleared first or the
 * controller refuses to launch (a blocked CCIF), exactly as on silicon. */
static void launch(uint32_t cmd)
{
    wait_ccif();
    FMU_FSTAT = ERRORS;      /* W1C any stale error flags */
    FMU_FCCOB0 = cmd;
    FMU_FSTAT = CCIF;        /* W1C CCIF = go */
}

/*
 * Erase the sector containing addr.  The sector is selected by the phrase the
 * guest writes during the PEWEN window — there is no address in FCCOB.
 */
static int erase_sector(uint32_t addr)
{
    volatile uint32_t *p = (volatile uint32_t *)(addr & ~0xFu);

    launch(ERSSCR);
    wait_pewen();
    for (int i = 0; i < PHRASE_WORDS; i++) {
        p[i] = 0;            /* content is irrelevant; the address selects */
    }
    wait_perdy();
    FMU_FSTAT = PERDY;       /* W1C PERDY = commit */
    wait_ccif();
    return !(FMU_FSTAT & (ERRORS));
}

/* Program n words (a phrase or a page) at addr; returns 0 if the FMU flagged
 * an error (which includes the program-verify failure). */
static int program(uint32_t addr, const uint32_t *w, int n)
{
    volatile uint32_t *p = (volatile uint32_t *)addr;

    launch(n == PAGE_WORDS ? PGMPG : PGMPHR);
    wait_pewen();
    for (int i = 0; i < n; i++) {
        p[i] = w[i];         /* the stores carry both address and data */
    }
    wait_perdy();
    FMU_FSTAT = PERDY;
    wait_ccif();
    return !(FMU_FSTAT & ERRORS);
}

/* Read-1s (verify erased): returns 1 if the region reads as erased. */
static int verify_erased(uint32_t cmd, uint32_t addr)
{
    wait_ccif();
    FMU_FSTAT = ERRORS;
    FMU_FCCOB0 = cmd;
    FMU_FCCOB2 = addr;       /* the address parameter is FCCOB2, not FCCOB1 */
    FMU_FSTAT = CCIF;
    wait_ccif();
    return !(FMU_FSTAT & FAIL);
}

void cpu0_main(void)
{
    volatile uint32_t *a = (volatile uint32_t *)PHRASE_A;
    volatile uint32_t *b = (volatile uint32_t *)PHRASE_B;
    volatile uint32_t *c = (volatile uint32_t *)PAGE_C;
    /* patB asks for 1-bits where patA has 0-bits, so a cumulative program
     * cannot succeed: flash only clears bits. */
    static const uint32_t patA[PHRASE_WORDS] =
        { 0xDEADBEEFu, 0xCAFEBABEu, 0x0F0F0F0Fu, 0x12345678u };
    static const uint32_t patB[PHRASE_WORDS] =
        { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xF0F0F0F0u, 0xFFFFFFFFu };
    uint32_t page[PAGE_WORDS];
    int ok = 1;

    LP_CTRL = (1u << 19);
    puts_("FMU test\r\n");

    for (int i = 0; i < PAGE_WORDS; i++) {
        page[i] = 0xA5000000u | (uint32_t)i;
    }

    /* --- erase, and prove the sector really is erased ------------------- */
    ok &= erase_sector(SECTOR);
    ok &= verify_erased(RD1SCR, SECTOR);
    for (int i = 0; i < PHRASE_WORDS; i++) {
        ok &= (a[i] == 0xFFFFFFFFu);
    }

    /* --- NEG1: a store outside a program window must do nothing --------- */
    a[0] = 0x5A5A5A5Au;              /* flash is not RAM */
    ok &= (a[0] == 0xFFFFFFFFu);     /* ...so this must NOT have landed */

    /* --- the round trip: program a phrase, read it back byte-exact ------ */
    ok &= program(PHRASE_A, patA, PHRASE_WORDS);
    for (int i = 0; i < PHRASE_WORDS; i++) {
        ok &= (a[i] == patA[i]);     /* byte-exact readback */
    }

    /* --- NEG3: the phrase no longer verifies as erased ------------------ */
    ok &= !verify_erased(RD1PHR, PHRASE_A);

    /* --- NEG2: cumulative program without an erase must fail ------------ */
    ok &= !program(PHRASE_A, patB, PHRASE_WORDS);   /* expect FAIL */
    for (int i = 0; i < PHRASE_WORDS; i++) {
        ok &= (a[i] == (patA[i] & patB[i]));        /* bits only ever cleared */
    }

    /* --- a full 128-byte page round trip in the same sector ------------- */
    ok &= program(PAGE_C, page, PAGE_WORDS);
    for (int i = 0; i < PAGE_WORDS; i++) {
        ok &= (c[i] == page[i]);
    }

    /* --- an untouched phrase in the sector is still erased -------------- */
    for (int i = 0; i < PHRASE_WORDS; i++) {
        ok &= (b[i] == 0xFFFFFFFFu);
    }

    /* --- erase again puts the whole sector back to 0xFF ----------------- */
    ok &= erase_sector(SECTOR);
    ok &= verify_erased(RD1SCR, SECTOR);
    for (int i = 0; i < PAGE_WORDS; i++) {
        ok &= (c[i] == 0xFFFFFFFFu);
    }

    puts_(ok ? "FMU PASS\r\n" : "FMU FAIL\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
