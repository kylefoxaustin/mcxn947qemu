/*
 * MCXN947 I3C controller data-path test — bytes really cross the bus.
 *
 * This block used to move NO DATA: MCTRL simply raised MCTRLDONE|COMPLETE and
 * MRDATAB read back the zeroed backing store, while the capability table claimed
 * "I3C master transfer".  The old version of this test asserted only that an
 * interrupt fired, so replacing MRDATAB with 0xA5A5A5A5 left it green — it could
 * not fail in the dimension it existed to protect.  The claim was retracted; this
 * is the data path being earned back.
 *
 * The controller now drives a REAL I2C bus (I3C is I2C-compatible in legacy mode).
 * The model supplies the BUS, as the silicon does — it does NOT invent a device
 * onto it.  The test attaches a real QEMU at24c EEPROM:
 *
 *     -device at24c-eeprom,bus=i2c-bus.0,address=0x50,rom-size=256
 *
 * which is exactly how a board wires a part to these pins.
 *
 * The golden is the EEPROM itself: bytes written must read back BIT-FOR-BIT.
 * Nothing in the I3C model can produce them by accident, because the data lives in
 * a device the I3C model does not own and cannot see.
 *
 * It also pins what a register file cannot express:
 *
 *   NEG1  addressing a target THAT IS NOT THERE must NACK (MERRWARN), not report
 *         a completed transfer.  A model that always "completes" is telling
 *         firmware it talked to a chip that does not exist.
 *
 * Prints "I3C PASS" only if every check holds.
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
static void putc_(char c){ while(!(LP_STAT&STAT_TDRE)){} LP_DATA=(uint8_t)c; }
static void puts_(const char*s){ while(*s) putc_(*s++); }

#define I3C0 0x40021000u
#define I3C_MCONFIG   (*(volatile uint32_t *)(I3C0 + 0x000))
#define I3C_MCTRL     (*(volatile uint32_t *)(I3C0 + 0x084))
#define I3C_MSTATUS   (*(volatile uint32_t *)(I3C0 + 0x088))
#define I3C_MERRWARN  (*(volatile uint32_t *)(I3C0 + 0x09C))
#define I3C_MWDATAB   (*(volatile uint32_t *)(I3C0 + 0x0B0))
#define I3C_MRDATAB   (*(volatile uint32_t *)(I3C0 + 0x0C0))

/* MCTRL (CMSIS I3C_MCTRL_*) */
#define REQ_EMITSTARTADDR 1u
#define REQ_EMITSTOP      2u
#define MCTRL_DIR_READ    (1u << 8)
#define MCTRL_ADDR(a)     ((uint32_t)(a) << 9)
#define MCTRL_RDTERM(n)   ((uint32_t)(n) << 16)

/* MSTATUS / MERRWARN */
#define MSTATUS_MCTRLDONE (1u << 9)
#define MSTATUS_COMPLETE  (1u << 10)
#define MERRWARN_NACK     (1u << 2)

#define EEPROM_ADDR 0x50
#define ABSENT_ADDR 0x21      /* nothing is wired at this address */
#define NBYTES 6

static void i3c_stop(void)
{
    I3C_MCTRL = REQ_EMITSTOP;
}

/* Start a transfer.  Returns 0 if the target did not acknowledge its address. */
static int i3c_start(uint32_t addr, int read, uint32_t nread)
{
    I3C_MERRWARN = 0xFFFFFFFFu;                /* W1C any stale error */
    I3C_MSTATUS = MSTATUS_MCTRLDONE | MSTATUS_COMPLETE;
    I3C_MCTRL = REQ_EMITSTARTADDR | MCTRL_ADDR(addr) |
                (read ? MCTRL_DIR_READ : 0) | MCTRL_RDTERM(nread);
    return !(I3C_MERRWARN & MERRWARN_NACK);
}

void cpu0_main(void)
{
    /* Bytes nothing in the I3C model could invent. */
    static const uint8_t data[NBYTES] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x5A, 0xA5 };
    uint8_t got[NBYTES];
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE;
    puts_("I3C test\r\n");

    I3C_MCONFIG = 1;                           /* controller enabled */

    /* --- NEG1: a target that is not there must NACK, not "complete" ------- */
    ok &= !i3c_start(ABSENT_ADDR, 0, 0);       /* must NOT be acknowledged */
    ok &= !!(I3C_MERRWARN & MERRWARN_NACK);
    i3c_stop();
    I3C_MERRWARN = 0xFFFFFFFFu;

    /* --- write NBYTES into the EEPROM at offset 0 ------------------------- */
    ok &= i3c_start(EEPROM_ADDR, 0, 0);        /* must be acknowledged */
    I3C_MWDATAB = 0x00;                        /* EEPROM byte address */
    for (i = 0; i < NBYTES; i++) {
        I3C_MWDATAB = data[i];
    }
    i3c_stop();

    /* --- point the EEPROM back at offset 0, then read the bytes out ------- */
    ok &= i3c_start(EEPROM_ADDR, 0, 0);
    I3C_MWDATAB = 0x00;
    i3c_stop();

    ok &= i3c_start(EEPROM_ADDR, 1, NBYTES);   /* read NBYTES */
    for (i = 0; i < NBYTES; i++) {
        got[i] = (uint8_t)I3C_MRDATAB;
    }
    i3c_stop();

    /* The golden lives in a device the I3C model does not own. */
    for (i = 0; i < NBYTES; i++) {
        ok &= (got[i] == data[i]);
    }

    puts_(ok ? "I3C PASS\r\n" : "I3C FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
