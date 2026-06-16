/*
 * MCXN947 SAI (audio) TX FIFO-request + NVIC interrupt test.
 *
 * Enables the SAI0 transmitter (TCSR.TE) together with the FIFO-request
 * interrupt (TCSR.FRIE) and the SAI0 NVIC line (IRQ 59).  The transmit FIFO
 * always reports space, so the FIFO-request flag (FRF) is asserted and, with
 * FRIE enabled, the model raises the interrupt - the "FIFO needs data" event a
 * real audio driver services by writing samples.  The ISR masks FRIE (as a
 * driver would once it has refilled) so the request does not storm.  Prints
 * "SAI PASS" once the TX FIFO-request interrupt is delivered through the
 * TCSR -> FRF -> NVIC -> handler path.
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

#define SAI0 0x40106000u
#define SAI_TCSR (*(volatile uint32_t *)(SAI0 + 0x08))

#define TCSR_FRIE (1u << 8)    /* FIFO request interrupt enable */
#define TCSR_TE   (1u << 31)   /* transmitter enable            */

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define SAI0_IRQ 59

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

void sai0_handler(void)
{
    irq_count++;
    SAI_TCSR = TCSR_TE;   /* mask FRIE (driver would refill FIFO) to stop storm */
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("SAI test\r\n");

    NVIC_ISER1 = (1u << (SAI0_IRQ - 32));
    __asm__ volatile ("cpsie i");

    /* Enable transmitter + FIFO-request interrupt: FIFO has space -> IRQ. */
    SAI_TCSR = TCSR_TE | TCSR_FRIE;

    while (irq_count < 1) {
    }

    puts_(irq_count >= 1 ? "SAI PASS\r\n" : "SAI FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + SAI0_IRQ] = sai0_handler,  /* exception 75 = IRQ 59 */
};
