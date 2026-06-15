/*
 * MCXN947 CTIMER + NVIC interrupt smoke test.
 *
 * Configures CTIMER0 for a periodic match (MR0 with interrupt + reset),
 * enables its NVIC line (IRQ 31), and counts the resulting interrupts in the
 * ISR.  Prints "CTIMER PASS" once several interrupts have been delivered,
 * exercising the full timer -> IR -> NVIC -> handler path.
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

#define CT0 0x4000C000u
#define CT_IR  (*(volatile uint32_t *)(CT0 + 0x00))
#define CT_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT_PR  (*(volatile uint32_t *)(CT0 + 0x0C))
#define CT_MCR (*(volatile uint32_t *)(CT0 + 0x14))
#define CT_MR0 (*(volatile uint32_t *)(CT0 + 0x18))

#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)  /* IRQ 0..31 enable */
#define CTIMER0_IRQ 31

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

static volatile uint32_t irq_count;

void ctimer0_handler(void)
{
    CT_IR = 0x1;        /* clear MR0 interrupt flag (write-1-to-clear) */
    irq_count++;
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("CTIMER test\r\n");

    CT_PR  = 0;                 /* count every timer clock */
    CT_MR0 = 1000;             /* match every 1000 counts */
    CT_MCR = (1u << 0) | (1u << 1);  /* MR0I (interrupt) + MR0R (reset on match) */
    CT_IR  = 0xFF;             /* clear stale flags */

    NVIC_ISER0 = (1u << CTIMER0_IRQ);
    __asm__ volatile ("cpsie i");

    CT_TCR = 0x1;              /* CEN: start the timer */

    while (irq_count < 5) {
    }

    puts_(irq_count >= 5 ? "CTIMER PASS\r\n" : "CTIMER FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[64] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + CTIMER0_IRQ] = ctimer0_handler,  /* exception 47 = IRQ 31 */
};
