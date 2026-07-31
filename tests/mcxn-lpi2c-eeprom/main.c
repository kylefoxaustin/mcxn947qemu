/*
 * LPI2C MASTER -> a REAL at24c EEPROM.
 *
 * FlexComm0 is switched to its LPI2C function (PSELID[PERSEL]=LPI2C) and used as an I2C
 * master to WRITE two bytes to an EEPROM and READ them back.  The EEPROM is a genuine
 * QEMU `at24c-eeprom` attached to this FlexComm's I2C bus on the command line -- the model
 * supplies only the BUS, so every byte read back comes from the EEPROM's own storage, an
 * oracle the LPI2C model cannot fabricate.
 *
 *   ⭐ THIS REPLACES A FABRICATION.  The LPI2C used to have NO bus: a "tiny echo target"
 *      ACKed every address and returned the last byte transmitted, so a read-after-write
 *      always "worked" -- the model was its own oracle (the uSDHC bug class).  A real
 *      EEPROM at a real address is the fix.
 *
 * Also negative-tested: addressing a device that is NOT on the bus sets MSR[NDF] (NACK) --
 * a real bus condition the echo target could never produce (it ACKed everything).
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

/* ---- FlexComm0 LPI2C (base 0x40092000, LPI2C sub-block at +0x800) --------- */
#define FC0   0x40092000u
#define PSELID (*(volatile uint32_t *)(FC0 + 0xFF8))
#define I2C   (FC0 + 0x800u)
#define MCR   (*(volatile uint32_t *)(I2C + 0x10))
#define MSR   (*(volatile uint32_t *)(I2C + 0x14))
#define MTDR  (*(volatile uint32_t *)(I2C + 0x60))   /* WO: CMD[10:8] | DATA[7:0] */
#define MRDR  (*(volatile uint32_t *)(I2C + 0x70))   /* RO: DATA | RXEMPTY(b14)   */
#define PERSEL_LPI2C 3u
#define MCR_MEN  (1u << 0)
#define MSR_TDF  (1u << 0)
#define MSR_RDF  (1u << 1)
#define MSR_NDF  (1u << 10)
#define MSR_W1C  (0x100u | 0x200u | 0x400u)          /* EPF | SDF | NDF */

/* MTDR commands. */
#define CMD_TX    (0u << 8)
#define CMD_RX    (1u << 8)
#define CMD_STOP  (2u << 8)
#define CMD_START (4u << 8)

#define EE_ADDR 0x50u

static void tdf(void)
{
    while (!(MSR & MSR_TDF)) {
    }
}

static void i2c_start(uint8_t addr7, int read)
{
    tdf();
    MTDR = CMD_START | ((uint32_t)(addr7 << 1) | (read ? 1u : 0u));
}
static void i2c_tx(uint8_t b) { tdf(); MTDR = CMD_TX | b; }
static void i2c_rxn(uint8_t n) { tdf(); MTDR = CMD_RX | (uint32_t)(n - 1); }  /* receive n */
static void i2c_stop(void) { tdf(); MTDR = CMD_STOP; }
static uint8_t i2c_read(void)
{
    while (!(MSR & MSR_RDF)) {
    }
    return MRDR & 0xFF;
}

void cpu0_main(void)
{
    int ok = 1;
    uint8_t b0, b1;

    LP_CTRL = CTRL_TE;
    puts_("LPI2C-EEPROM test\r\n");

    /* Switch FlexComm0 to its LPI2C function and enable the master. */
    PSELID = PERSEL_LPI2C;
    MCR = MCR_MEN;

    /* WRITE 0xAB, 0xCD at EEPROM offset 0x10. */
    i2c_start(EE_ADDR, 0);
    i2c_tx(0x10);                 /* word address */
    i2c_tx(0xAB);
    i2c_tx(0xCD);
    i2c_stop();
    if (MSR & MSR_NDF) {
        puts_("  FAIL: EEPROM NACKed the write\r\n");
        ok = 0;
    }
    MSR = MSR_W1C;               /* clear EPF/SDF */

    /* READ them back: dummy write of the offset, repeated START for read, 2 bytes. */
    i2c_start(EE_ADDR, 0);
    i2c_tx(0x10);
    i2c_start(EE_ADDR, 1);       /* repeated START, read direction */
    i2c_rxn(2);
    b0 = i2c_read();
    b1 = i2c_read();
    i2c_stop();

    puts_("  read back: "); puthex2(b0); putc_(' '); puthex2(b1); puts_("\r\n");
    ok &= (b0 == 0xAB);
    ok &= (b1 == 0xCD);
    ok &= !(MSR & MSR_NDF);
    puts_(ok ? "  readback matches the EEPROM (correct)\r\n"
             : "  readback FAILED\r\n");

    /* NEGATIVE: address a device that is NOT on the bus -> NACK (MSR[NDF]). */
    MSR = MSR_W1C;
    i2c_start(0x51, 0);          /* nothing at 0x51 */
    i2c_stop();
    if (MSR & MSR_NDF) {
        puts_("  absent device -> NDF (correct)\r\n");
    } else {
        puts_("  FAIL: absent device did not NACK\r\n");
        ok = 0;
    }

    puts_(ok ? "LPI2C-EEPROM PASS\r\n" : "LPI2C-EEPROM FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
