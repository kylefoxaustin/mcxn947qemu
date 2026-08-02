/*
 * LPSPI MASTER -> a REAL m25p80 SPI-NOR.
 *
 * FlexComm1 is switched to its LPSPI function (PSELID[PERSEL]=LPSPI) and used as an SPI
 * master to talk to a genuine QEMU `w25q64` NOR flash that the SoC wires onto FlexComm1's
 * SSI bus (with its chip-select connected).  Two checks, both against the FLASH as oracle:
 *
 *   1. RDID (0x9F): read the JEDEC ID -> 0xEF 0x40 0x17 (Winbond W25Q64).  A fixed value
 *      that comes from the flash model, not the controller.
 *   2. WREN + PAGE-PROGRAM + READ: write two bytes and read them back byte-exact -- the
 *      data round-trips through the flash's own storage, and the WREN latch / erase-state
 *      physics are the flash's, not faked.
 *
 *   ⭐ This replaces a LOOPBACK: with no device the LPSPI TDR looped straight into RDR
 *      (a MOSI->MISO jumper), so a "transfer" returned its own bytes -- the model was its
 *      own oracle.  A real NOR at a real chip-select is the fix.
 *
 * The chip-select is held asserted across each multi-byte command via TCR[CONT], dropped
 * on the final frame -- exactly how LPSPI drives a NOR on silicon.
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

static void puthex2(uint8_t v)
{
    const char *H = "0123456789ABCDEF";

    putc_(H[(v >> 4) & 0xF]);
    putc_(H[v & 0xF]);
}

/* ---- FlexComm1 LPSPI (base 0x40093000) ----------------------------------- */
#define FC1   0x40093000u
#define PSELID  (*(volatile uint32_t *)(FC1 + 0xFF8))
#define SPI_CR  (*(volatile uint32_t *)(FC1 + 0x10))
#define SPI_SR  (*(volatile uint32_t *)(FC1 + 0x14))
#define SPI_TCR (*(volatile uint32_t *)(FC1 + 0x60))
#define SPI_TDR (*(volatile uint32_t *)(FC1 + 0x64))   /* WO */
#define SPI_RDR (*(volatile uint32_t *)(FC1 + 0x74))   /* RO */
#define PERSEL_LPSPI 2u
#define CR_MEN   (1u << 0)
#define SR_RDF   (1u << 1)
#define TCR_FRAMESZ8 7u                                /* FRAMESZ+1 = 8 bits */
#define TCR_CONT (1u << 21)

/* One 8-bit SPI frame: TX a byte, return the byte shifted in.  cont=1 holds CS asserted. */
static uint8_t spi(uint8_t tx, int cont)
{
    SPI_TCR = TCR_FRAMESZ8 | (cont ? TCR_CONT : 0u);
    SPI_TDR = tx;
    while (!(SPI_SR & SR_RDF)) {
    }
    return SPI_RDR & 0xFF;
}

/* ---- m25p80 (w25q64) commands -------------------------------------------- */
#define NOR_RDID 0x9Fu
#define NOR_WREN 0x06u
#define NOR_PP   0x02u
#define NOR_READ 0x03u

void cpu0_main(void)
{
    int ok = 1;
    uint8_t id0, id1, id2, b0, b1;

    LP_CTRL = CTRL_TE;
    puts_("LPSPI-NOR test\r\n");

    PSELID = PERSEL_LPSPI;
    SPI_CR = CR_MEN;

    /* 1) JEDEC ID. */
    spi(NOR_RDID, 1);
    id0 = spi(0, 1);
    id1 = spi(0, 1);
    id2 = spi(0, 0);             /* last frame -> release CS */
    puts_("  JEDEC ID: "); puthex2(id0); putc_(' '); puthex2(id1); putc_(' ');
    puthex2(id2); puts_("\r\n");
    ok &= (id0 == 0xEF && id1 == 0x40 && id2 == 0x17);   /* Winbond W25Q64 */

    /* 2) WREN, PAGE-PROGRAM 0x5A,0xA5 at address 0x000100, then READ back. */
    spi(NOR_WREN, 0);
    spi(NOR_PP, 1);
    spi(0x00, 1); spi(0x01, 1); spi(0x00, 1);   /* 24-bit address 0x000100 */
    spi(0x5A, 1);
    spi(0xA5, 0);               /* last data byte -> release CS (starts the program) */

    spi(NOR_READ, 1);
    spi(0x00, 1); spi(0x01, 1); spi(0x00, 1);   /* address 0x000100 */
    b0 = spi(0, 1);
    b1 = spi(0, 0);
    puts_("  read back: "); puthex2(b0); putc_(' '); puthex2(b1); puts_("\r\n");
    ok &= (b0 == 0x5A && b1 == 0xA5);

    puts_(ok ? "LPSPI-NOR PASS\r\n" : "LPSPI-NOR FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
