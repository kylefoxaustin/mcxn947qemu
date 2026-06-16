/*
 * MCXN947 uSDHC SD command/response + NVIC interrupt test.
 *
 * Software-resets the host, enables the command-complete interrupt signal and
 * the uSDHC NVIC line (IRQ 61), then runs the start of an SD enumeration:
 *   CMD8 (SEND_IF_COND, arg 0x1AA) - the model echoes the voltage + check
 *        pattern in CMD_RSP0 (R7), the canonical "is this SD 2.0?" probe.
 *   CMD3 (SEND_RELATIVE_ADDR) - the model returns the assigned RCA in
 *        CMD_RSP0 bits 31:16 (R6).
 * Each command raises command-complete and delivers an interrupt; the ISR
 * counts and acks them.  Prints "USDHC PASS" when both responses are correct
 * and both interrupts were delivered through the command -> CC -> NVIC ->
 * handler path.
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

#define USDHC 0x40109000u
#define CMD_ARG      (*(volatile uint32_t *)(USDHC + 0x08))
#define CMD_XFR_TYP  (*(volatile uint32_t *)(USDHC + 0x0C))
#define CMD_RSP0     (*(volatile uint32_t *)(USDHC + 0x10))
#define SYS_CTRL     (*(volatile uint32_t *)(USDHC + 0x2C))
#define INT_STATUS   (*(volatile uint32_t *)(USDHC + 0x30))
#define INT_SIGNAL_EN (*(volatile uint32_t *)(USDHC + 0x38))

#define SYS_CTRL_RSTA (1u << 24)
#define INT_CC        (1u << 0)
#define CMD(idx)      ((uint32_t)(idx) << 24)

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define USDHC_IRQ 61

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

static volatile uint32_t cc_count;

void usdhc_handler(void)
{
    uint32_t st = INT_STATUS;
    if (st & INT_CC) {
        cc_count++;
        INT_STATUS = INT_CC;   /* write-1-to-clear */
    }
}

void cpu0_main(void)
{
    uint32_t rsp8, rsp3;

    LP_CTRL = CTRL_TE;
    puts_("USDHC test\r\n");

    SYS_CTRL = SYS_CTRL_RSTA;        /* software reset, self-clears */
    while (SYS_CTRL & SYS_CTRL_RSTA) {
    }

    INT_SIGNAL_EN = INT_CC;
    NVIC_ISER1 = (1u << (USDHC_IRQ - 32));
    __asm__ volatile ("cpsie i");

    /* CMD8 SEND_IF_COND: voltage 2.7-3.6V (0x100) + check pattern 0xAA. */
    CMD_ARG = 0x1AA;
    CMD_XFR_TYP = CMD(8);
    while (cc_count < 1) {
    }
    rsp8 = CMD_RSP0;

    /* CMD3 SEND_RELATIVE_ADDR: card returns its RCA. */
    CMD_ARG = 0;
    CMD_XFR_TYP = CMD(3);
    while (cc_count < 2) {
    }
    rsp3 = CMD_RSP0;

    if (rsp8 == 0x1AA && (rsp3 >> 16) == 0x0001 && cc_count >= 2) {
        puts_("USDHC PASS\r\n");
    } else {
        puts_("USDHC FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + USDHC_IRQ] = usdhc_handler,  /* exception 77 = IRQ 61 */
};
