/*
 * FlexIO AS AN SPI MASTER -> a REAL m25p80 SPI-NOR.
 *
 * FlexIO is a bare shifter/timer/pin fabric; software configures it to BE an SPI master:
 * shifter 0 in transmit mode drives MOSI, shifter 1 in receive mode captures MISO, a timer
 * clocks SCK, and a FlexIO output pin drives the chip-select.  Here it drives a genuine
 * QEMU w25q64 NOR wired onto the FlexIO SSI bus.
 *
 *   1. RDID (0x9F): JEDEC ID -> 0xEF 0x40 0x17 (from the flash, not the fabric).
 *   2. WREN + PAGE-PROGRAM + READ: two bytes round-trip through the flash byte-exact.
 *
 * The chip-select is held asserted (FlexIO pin 4 low) for the whole command and released
 * after -- the board wires FlexIO pin 4 to the NOR's CS.  A byte written to shifter 0's
 * buffer (SHIFTBUFBIS, MSB-first) is shifted onto the bus; the byte shifted in appears in
 * shifter 1's buffer, flagged by SHIFTSTAT.
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

/* ---- FlexIO0 (base 0x40105000) ------------------------------------------- */
#define FIO 0x40105000u
#define FIO_CTRL      (*(volatile uint32_t *)(FIO + 0x008))
#define FIO_SHIFTSTAT (*(volatile uint32_t *)(FIO + 0x010))
#define FIO_SHIFTCTL(i)    (*(volatile uint32_t *)(FIO + 0x080 + (i) * 4))
#define FIO_SHIFTCFG(i)    (*(volatile uint32_t *)(FIO + 0x100 + (i) * 4))
#define FIO_SHIFTBUFBIS(i) (*(volatile uint32_t *)(FIO + 0x280 + (i) * 4))
#define FIO_TIMCTL(i)      (*(volatile uint32_t *)(FIO + 0x400 + (i) * 4))
#define FIO_PINOUTD   (*(volatile uint32_t *)(FIO + 0x060))
#define FIO_PINOUTCLR (*(volatile uint32_t *)(FIO + 0x06C))
#define FIO_PINOUTSET (*(volatile uint32_t *)(FIO + 0x070))
#define FLEXEN   (1u << 0)
#define SMOD_TX  2u
#define SMOD_RX  1u
#define CS_PIN   4u

/* SHIFTCTL: SMOD[2:0] | PINSEL[12:8] | TIMSEL[26:24]. */
#define SHIFTCTL(smod, pin, tim) ((smod) | ((pin) << 8) | ((tim) << 24))

static uint8_t spi(uint8_t tx)
{
    FIO_SHIFTBUFBIS(0) = tx;                 /* transmit shifter, MSB-first */
    while (!(FIO_SHIFTSTAT & (1u << 1))) {   /* wait for the receive shifter */
    }
    return FIO_SHIFTBUFBIS(1) & 0xFF;
}
static void cs_lo(void) { FIO_PINOUTCLR = (1u << CS_PIN); }   /* assert (active low)   */
static void cs_hi(void) { FIO_PINOUTSET = (1u << CS_PIN); }   /* release               */

#define NOR_RDID 0x9Fu
#define NOR_WREN 0x06u
#define NOR_PP   0x02u
#define NOR_READ 0x03u

void cpu0_main(void)
{
    int ok = 1;
    uint8_t id0, id1, id2, b0, b1;

    LP_CTRL = CTRL_TE;
    puts_("FLEXIO-SPI-NOR test\r\n");

    /* Configure FlexIO as an SPI master: TX shifter 0 (MOSI), RX shifter 1 (MISO), timer 0. */
    FIO_CTRL = FLEXEN;
    FIO_SHIFTCTL(0) = SHIFTCTL(SMOD_TX, 0, 0);   /* transmit -> MOSI (pin 0), timer 0 */
    FIO_SHIFTCTL(1) = SHIFTCTL(SMOD_RX, 1, 0);   /* receive  <- MISO (pin 1), timer 0 */
    FIO_SHIFTCFG(0) = 0;
    FIO_SHIFTCFG(1) = 0;
    FIO_TIMCTL(0)   = (1u << 8);                  /* a timer for SCK (config only)     */
    FIO_PINOUTD     = (1u << CS_PIN);             /* CS released (high) to start       */

    /* 1) JEDEC ID. */
    cs_lo();
    spi(NOR_RDID);
    id0 = spi(0); id1 = spi(0); id2 = spi(0);
    cs_hi();
    puts_("  JEDEC ID: "); puthex2(id0); putc_(' '); puthex2(id1); putc_(' ');
    puthex2(id2); puts_("\r\n");
    ok &= (id0 == 0xEF && id1 == 0x40 && id2 == 0x17);

    /* 2) WREN + PAGE-PROGRAM 0x3C,0xC3 at 0x000200, then READ back. */
    cs_lo(); spi(NOR_WREN); cs_hi();
    cs_lo();
    spi(NOR_PP); spi(0x00); spi(0x02); spi(0x00);   /* 24-bit address 0x000200 */
    spi(0x3C); spi(0xC3);
    cs_hi();

    cs_lo();
    spi(NOR_READ); spi(0x00); spi(0x02); spi(0x00);
    b0 = spi(0); b1 = spi(0);
    cs_hi();
    puts_("  read back: "); puthex2(b0); putc_(' '); puthex2(b1); puts_("\r\n");
    ok &= (b0 == 0x3C && b1 == 0xC3);

    puts_(ok ? "FLEXIO-SPI-NOR PASS\r\n" : "FLEXIO-SPI-NOR FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
