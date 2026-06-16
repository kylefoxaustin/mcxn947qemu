/*
 * MCXN947 DAC FIFO-watermark + NVIC interrupt test.
 *
 * The DAC output FIFO is always drained in the model, so the watermark flag
 * (FSR.WM, "room available") is asserted.  Enabling the watermark interrupt
 * (IER.WM_IE) and the DAC0 NVIC line (IRQ 106) therefore raises the "FIFO
 * needs samples" interrupt a real DAC driver services by writing DATA.  The
 * ISR masks IER (as a driver would once it has refilled) so the request does
 * not storm.  Prints "DAC PASS" once the watermark interrupt is delivered
 * through the FSR.WM & IER.WM_IE -> NVIC -> handler path.
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

#define DAC0 0x4010F000u
#define DAC_IER (*(volatile uint32_t *)(DAC0 + 0x1C))

#define IER_WM_IE (1u << 2)   /* FIFO watermark interrupt enable */

#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)  /* IRQ 96..127 */
#define DAC0_IRQ 106

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

void dac0_handler(void)
{
    irq_count++;
    DAC_IER = 0;   /* mask the watermark request (driver would refill) */
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("DAC test\r\n");

    NVIC_ISER3 = (1u << (DAC0_IRQ - 96));
    __asm__ volatile ("cpsie i");

    DAC_IER = IER_WM_IE;   /* FIFO has room -> watermark interrupt fires */

    while (irq_count < 1) {
    }

    puts_(irq_count >= 1 ? "DAC PASS\r\n" : "DAC FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[130] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + DAC0_IRQ] = dac0_handler,  /* exception 122 = IRQ 106 */
};
