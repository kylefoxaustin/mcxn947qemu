/*
 * MCXN947 LP_FLEXCOMM SPI + I2C function test.
 *
 * Each FlexComm block is one LP_FLEXCOMM that function-selects LPUART, LPSPI or
 * LPI2C via PSELID.PERSEL.  This test drives two non-console FlexComms:
 *
 *   - FlexComm3 @ 0x40095000 (IRQ 38) as an LPSPI master: enable, issue an
 *     8-bit frame by writing TDR, the model loops the word back into the RX
 *     FIFO (physical MOSI->MISO jumper), sets WCF/FCF/TCF + RDF and raises the
 *     shared FlexComm IRQ.  The ISR reads RDR and checks it equals the TX word.
 *
 *   - FlexComm0 @ 0x40092000 (IRQ 35) as an LPI2C controller: enable, WRITE a byte to
 *     a REAL at24c EEPROM (attached to flexcomm0-i2c), then READ it back -- the byte
 *     round-trips through the EEPROM's storage, not a fabricated echo -- then STOP,
 *     which sets SDF|EPF and raises the IRQ.
 *
 * Prints "FLEXCOMM PASS" via the FlexComm4 console once both the SPI loopback
 * word and the I2C byte read back from the EEPROM arrive AND both shared FlexComm
 * interrupts have been delivered through the NVIC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

/* --- FlexComm4 console (LPUART) -------------------------------------------- */
#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

/* --- LP_FLEXCOMM wrapper + PERSEL ------------------------------------------ */
#define FC_PSELID_OFF 0xFF8u
#define PERSEL_LPSPI  2u
#define PERSEL_LPI2C  3u

/* --- LPSPI on FlexComm3 ---------------------------------------------------- */
#define FC3_BASE   0x40095000u
#define SPI_CR     (*(volatile uint32_t *)(FC3_BASE + 0x10))
#define SPI_SR     (*(volatile uint32_t *)(FC3_BASE + 0x14))
#define SPI_IER    (*(volatile uint32_t *)(FC3_BASE + 0x18))
#define SPI_CFGR1  (*(volatile uint32_t *)(FC3_BASE + 0x24))
#define SPI_TCR    (*(volatile uint32_t *)(FC3_BASE + 0x60))
#define SPI_TDR    (*(volatile uint32_t *)(FC3_BASE + 0x64))
#define SPI_RDR    (*(volatile uint32_t *)(FC3_BASE + 0x74))
#define SPI_PSELID (*(volatile uint32_t *)(FC3_BASE + FC_PSELID_OFF))
#define SPI_CR_MEN     0x1u
#define SPI_SR_RDF     0x2u
#define SPI_SR_TCF     0x400u
#define SPI_IER_RDIE   0x2u
#define SPI_IER_TCIE   0x400u
#define SPI_CFGR1_MASTER 0x1u
#define FC3_IRQ 38
#define SPI_TX_WORD 0xA5u

/* --- LPI2C on FlexComm0 ---------------------------------------------------- */
#define FC0_BASE   0x40092000u

/*
 * ⭐ THE LPI2C SUB-BLOCK IS AT +0x800, AND THIS TEST USED TO SAY +0x000.
 *
 * CMSIS:  LP_FLEXCOMM0_BASE = 0x4009_2000
 *         LPUART0_BASE      = 0x4009_2000   (+0x000)
 *         LPSPI0_BASE       = 0x4009_2000   (+0x000)
 *         LPI2C0_BASE       = 0x4009_2800   (+0x800)   <-- not with the others
 *
 * The model ALSO had LPI2C at +0x000, so every LPI2C access from real firmware --
 * which of course uses LPI2C0_BASE -- landed in an unmapped hole and was silently
 * dropped.  The IP was UNREACHABLE FROM THE GUEST, and the stock lpi2c examples sat
 * there printing a banner and doing nothing.
 *
 * AND THIS TEST PASSED THROUGH ALL OF IT, because it took its addresses FROM THE
 * MODEL.  It poked +0x10, the model answered at +0x10, and the two of them agreed
 * perfectly about a register that does not exist at that address on the part.
 *
 *     ⭐ A TEST THAT GETS ITS ADDRESSES FROM THE MODEL IS NOT A TEST.  IT IS A
 *        MIRROR.  And mutation testing CANNOT SEE IT: mutate the model and the
 *        mirror moves with it, so the test still "catches" the mutation and still
 *        proves nothing about the silicon.
 *
 * It took an oracle I did not write -- the RM's reset values, read back at the
 * addresses CMSIS gives -- to notice that nobody was home.  Addresses now come from
 * CMSIS, like firmware's do.
 */
#define LPI2C0_BASE (FC0_BASE + 0x800u)
#define I2C_MCR    (*(volatile uint32_t *)(LPI2C0_BASE + 0x10))
#define I2C_MSR    (*(volatile uint32_t *)(LPI2C0_BASE + 0x14))
#define I2C_MIER   (*(volatile uint32_t *)(LPI2C0_BASE + 0x18))
#define I2C_MTDR   (*(volatile uint32_t *)(LPI2C0_BASE + 0x60))
#define I2C_MRDR   (*(volatile uint32_t *)(LPI2C0_BASE + 0x70))
#define I2C_PSELID (*(volatile uint32_t *)(FC0_BASE + FC_PSELID_OFF))
#define I2C_MCR_MEN   0x1u
#define I2C_MSR_RDF   0x2u
#define I2C_MSR_EPF   0x100u
#define I2C_MIER_RDIE 0x2u
#define I2C_MIER_EPIE 0x100u
#define I2C_MRDR_RXEMPTY 0x4000u
#define I2C_CMD(c, d) (((uint32_t)(c) << 8) | ((d) & 0xFFu))
#define I2C_CMD_TX    0u
#define I2C_CMD_RX    1u
#define I2C_CMD_STOP  2u
#define I2C_CMD_START 4u
#define FC0_IRQ 35
#define I2C_DEV_ADDR  0x50u
#define I2C_TX_BYTE   0x3Cu

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */

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

static volatile uint32_t spi_irq, i2c_irq;
/* Zero-init (.bss) and seeded in cpu0_main: there is no startup .data copy, so
 * an initialised writable global would read as zero.  See link.ld. */
static volatile uint32_t spi_rx, i2c_rx;

/* FlexComm3 shared IRQ — LPSPI transfer complete + RX data. */
void fc3_handler(void)
{
    if (SPI_SR & SPI_SR_RDF) {
        spi_rx = SPI_RDR & 0xFF;        /* reading RDR clears RDF */
    }
    SPI_SR = SPI_SR_TCF;                /* W1C transfer-complete */
    spi_irq++;
}

/* FlexComm0 shared IRQ — LPI2C end-packet. */
void fc0_handler(void)
{
    I2C_MSR = I2C_MSR_EPF;              /* W1C end-packet */
    i2c_irq++;
}

static void spi_test(void)
{
    SPI_PSELID = PERSEL_LPSPI;          /* function-select LPSPI */
    SPI_CFGR1 = SPI_CFGR1_MASTER;
    SPI_CR = SPI_CR_MEN;
    SPI_TCR = 7u;                       /* FRAMESZ=7 -> 8-bit frame */
    SPI_IER = SPI_IER_TCIE | SPI_IER_RDIE;
    NVIC_ISER1 = (1u << (FC3_IRQ - 32));

    SPI_TDR = SPI_TX_WORD;              /* master shift -> loopback */

    while (spi_irq < 1) {
    }
}

static void i2c_test(void)
{
    I2C_PSELID = PERSEL_LPI2C;          /* function-select LPI2C */
    I2C_MCR = I2C_MCR_MEN;
    I2C_MIER = I2C_MIER_EPIE;           /* completion IRQ only; RX is polled */
    NVIC_ISER1 = (1u << (FC0_IRQ - 32));

    /*
     * A REAL EEPROM transaction (a genuine at24c is attached to flexcomm0-i2c on the
     * command line) -- NOT the model's old echo target.  Write I2C_TX_BYTE to EEPROM
     * offset 0x20, then read it back: i2c_rx equals I2C_TX_BYTE because the byte round-trips
     * through the EEPROM's own storage, an oracle the model cannot fabricate.
     */
    I2C_MTDR = I2C_CMD(I2C_CMD_START, I2C_DEV_ADDR << 1);  /* START + addr (W) */
    I2C_MTDR = I2C_CMD(I2C_CMD_TX, 0x20);                  /* word address */
    I2C_MTDR = I2C_CMD(I2C_CMD_TX, I2C_TX_BYTE);           /* data */
    I2C_MTDR = I2C_CMD(I2C_CMD_STOP, 0);                   /* STOP -> SDF|EPF */

    I2C_MTDR = I2C_CMD(I2C_CMD_START, I2C_DEV_ADDR << 1);  /* START + addr (W) */
    I2C_MTDR = I2C_CMD(I2C_CMD_TX, 0x20);                  /* set the read pointer */
    I2C_MTDR = I2C_CMD(I2C_CMD_START, (I2C_DEV_ADDR << 1) | 1); /* repeated START (R) */
    I2C_MTDR = I2C_CMD(I2C_CMD_RX, 0);                     /* receive 1 byte */

    while (I2C_MSR & I2C_MSR_RDF) {
        uint32_t d = I2C_MRDR;
        if (!(d & I2C_MRDR_RXEMPTY)) {
            i2c_rx = d & 0xFF;
        }
        break;
    }

    I2C_MTDR = I2C_CMD(I2C_CMD_STOP, 0);                   /* STOP -> SDF|EPF */

    while (i2c_irq < 1) {
    }
}

void cpu0_main(void)
{
    spi_rx = 0xFFFF;
    i2c_rx = 0xFFFF;

    LP_CTRL = CTRL_TE;
    puts_("FLEXCOMM test\r\n");

    __asm__ volatile ("cpsie i");

    spi_test();
    i2c_test();

    if (spi_irq >= 1 && spi_rx == SPI_TX_WORD &&
        i2c_irq >= 1 && i2c_rx == I2C_TX_BYTE) {
        puts_("FLEXCOMM PASS\r\n");
    } else {
        puts_("FLEXCOMM FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[128] = {
    [0]  = (vec_t)0x20010000u,       /* initial MSP */
    [1]  = cpu0_main,                /* Reset_Handler */
    [16 + FC0_IRQ] = fc0_handler,    /* exception 51 = IRQ 35 (FlexComm0) */
    [16 + FC3_IRQ] = fc3_handler,    /* exception 54 = IRQ 38 (FlexComm3) */
};
