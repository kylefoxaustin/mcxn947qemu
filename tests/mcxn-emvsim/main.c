/*
 * MCXN947 EMVSIM transmit-complete interrupt test.
 *
 * Drives both EMVSIM0 (IRQ 103) and EMVSIM1 (IRQ 104).  For each: unmask the
 * transmit-complete interrupt (INT_MASK, where 0 = enabled), enable the NVIC
 * line, then write a byte to TX_BUF.  The model transmits synchronously and
 * latches TX_STATUS.TCF, raising the EMVSIM interrupt; the ISR W1C-clears
 * TX_STATUS to drop the line.  Prints "EMVSIM PASS" once both transmit-complete
 * interrupts are delivered through the NVIC.
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

#define EMVSIM0_BASE 0x40103000u
#define EMVSIM1_BASE 0x40104000u
#define EMV_INT_MASK 0x14u
#define EMV_TX_STATUS 0x24u
#define EMV_TX_BUF    0x30u
#define REG(base, off) (*(volatile uint32_t *)((base) + (off)))

#define INT_MASK_TC_IM 0x2u          /* clear to enable transmit-complete IRQ */
#define TX_STATUS_TCF  0x20u
#define TX_STATUS_CLR  0xFFu         /* W1C the latched TX flags */

#define EMVSIM0_IRQ 103
#define EMVSIM1_IRQ 104
#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)  /* IRQ 96..127 */

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

static volatile uint32_t irq0, irq1;

void emvsim0_handler(void)
{
    if (REG(EMVSIM0_BASE, EMV_TX_STATUS) & TX_STATUS_TCF) {
        irq0++;
    }
    REG(EMVSIM0_BASE, EMV_TX_STATUS) = TX_STATUS_CLR;
}

void emvsim1_handler(void)
{
    if (REG(EMVSIM1_BASE, EMV_TX_STATUS) & TX_STATUS_TCF) {
        irq1++;
    }
    REG(EMVSIM1_BASE, EMV_TX_STATUS) = TX_STATUS_CLR;
}

static void emvsim_tx(uint32_t base)
{
    REG(base, EMV_INT_MASK) = ~INT_MASK_TC_IM;   /* enable TC only */
    REG(base, EMV_TX_BUF) = 0x5Au;               /* transmit -> TCF -> IRQ */
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("EMVSIM test\r\n");

    NVIC_ISER3 = (1u << (EMVSIM0_IRQ - 96)) | (1u << (EMVSIM1_IRQ - 96));
    __asm__ volatile ("cpsie i");

    emvsim_tx(EMVSIM0_BASE);
    emvsim_tx(EMVSIM1_BASE);

    while (irq0 < 1 || irq1 < 1) {
    }

    puts_((irq0 >= 1 && irq1 >= 1) ? "EMVSIM PASS\r\n" : "EMVSIM FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[128] = {
    [0]  = (vec_t)0x20010000u,           /* initial MSP */
    [1]  = cpu0_main,                    /* Reset_Handler */
    [16 + EMVSIM0_IRQ] = emvsim0_handler, /* exception 119 = IRQ 103 */
    [16 + EMVSIM1_IRQ] = emvsim1_handler, /* exception 120 = IRQ 104 */
};
