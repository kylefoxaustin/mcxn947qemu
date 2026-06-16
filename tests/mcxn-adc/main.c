/*
 * MCXN947 ADC (LPADC) + NVIC interrupt smoke test.
 *
 * Enables ADC0, arms the FIFO-watermark interrupt (FWMIE0), enables the ADC0
 * NVIC line (IRQ 45), then fires a software trigger.  The model completes the
 * conversion, sets STAT[RDY0] and asserts IRQ 45; the ISR reads RESFIFO0,
 * verifies the VALID bit, captures the result, and clears the interrupt.
 * Prints "ADC PASS" once a valid result has been delivered through the full
 * ADC -> STAT -> NVIC -> handler path.
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

#define ADC0 0x4010D000u
#define ADC_CTRL    (*(volatile uint32_t *)(ADC0 + 0x010))
#define ADC_STAT    (*(volatile uint32_t *)(ADC0 + 0x014))
#define ADC_IE      (*(volatile uint32_t *)(ADC0 + 0x018))
#define ADC_SWTRIG  (*(volatile uint32_t *)(ADC0 + 0x034))
#define ADC_FCTRL0  (*(volatile uint32_t *)(ADC0 + 0x0E0))
#define ADC_RESFIFO0 (*(volatile uint32_t *)(ADC0 + 0x300))

#define CTRL_ADCEN   (1u << 0)
#define IE_FWMIE0    (1u << 0)
#define RESFIFO_VALID (1u << 31)

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 enable */
#define ADC0_IRQ 45

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
static volatile uint32_t last_result;

void adc0_handler(void)
{
    uint32_t r = ADC_RESFIFO0;   /* read clears STAT[RDY0] and deasserts IRQ */
    if (r & RESFIFO_VALID) {
        last_result = r & 0xFFFFu;
        irq_count++;
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("ADC test\r\n");

    ADC_CTRL = CTRL_ADCEN;       /* enable the converter */
    ADC_IE   = IE_FWMIE0;        /* FIFO-watermark interrupt enable */

    NVIC_ISER1 = (1u << (ADC0_IRQ - 32));
    __asm__ volatile ("cpsie i");

    ADC_SWTRIG = 0x1;            /* software trigger -> conversion completes */

    while (irq_count < 1) {
    }

    /* A second trigger to confirm the path re-arms cleanly. */
    ADC_SWTRIG = 0x1;
    while (irq_count < 2) {
    }

    if (irq_count >= 2 && last_result != 0) {
        puts_("ADC PASS\r\n");
    } else {
        puts_("ADC FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + ADC0_IRQ] = adc0_handler,  /* exception 61 = IRQ 45 */
};
