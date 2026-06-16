/*
 * MCXN947 PowerQuad (DSP coprocessor) compute-done + NVIC interrupt test.
 *
 * Enables the PowerQuad completion interrupt (INTREN) and the PQ NVIC line
 * (IRQ 76), then launches an instruction by writing CONTROL.  The model
 * retires the instruction instantly and raises the completion interrupt
 * (INTRSTAT); the ISR acks it.  Prints "POWERQUAD PASS" once the compute-done
 * interrupt is delivered through the CONTROL -> INTRSTAT -> NVIC -> handler
 * path.
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

#define PQ 0x400BF000u
#define PQ_CONTROL  (*(volatile uint32_t *)(PQ + 0x100))
#define PQ_INTREN   (*(volatile uint32_t *)(PQ + 0x190))
#define PQ_INTRSTAT (*(volatile uint32_t *)(PQ + 0x198))

#define INTR_EN   (1u << 0)
#define INTR_STAT (1u << 0)

#define NVIC_ISER2 (*(volatile uint32_t *)0xE000E108u)  /* IRQ 64..95 */
#define PQ_IRQ 76

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

void pq_handler(void)
{
    if (PQ_INTRSTAT & INTR_STAT) {
        irq_count++;
        PQ_INTRSTAT = INTR_STAT;   /* write-1-to-clear */
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("POWERQUAD test\r\n");

    PQ_INTREN = INTR_EN;
    NVIC_ISER2 = (1u << (PQ_IRQ - 64));
    __asm__ volatile ("cpsie i");

    /* Launch an instruction: writing CONTROL retires it and raises completion. */
    PQ_CONTROL = 0x00000001;

    while (irq_count < 1) {
    }

    puts_(irq_count >= 1 ? "POWERQUAD PASS\r\n" : "POWERQUAD FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[100] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + PQ_IRQ] = pq_handler,  /* exception 92 = IRQ 76 */
};
