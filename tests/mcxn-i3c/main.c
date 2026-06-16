/*
 * MCXN947 I3C controller transfer-complete + NVIC interrupt test.
 *
 * Enables the controller MCTRLDONE/COMPLETE interrupts (MINTSET) and the I3C0
 * NVIC line (IRQ 95), then issues a controller request by writing MCTRL.REQUEST.
 * The model completes the message immediately, sets MSTATUS.MCTRLDONE|COMPLETE
 * and raises the interrupt; the ISR acks it.  Prints "I3C PASS" once the
 * transfer-complete interrupt is delivered through the MCTRL -> MSTATUS ->
 * NVIC -> handler path.
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

#define I3C0 0x40021000u
#define I3C_MCTRL   (*(volatile uint32_t *)(I3C0 + 0x84))
#define I3C_MSTATUS (*(volatile uint32_t *)(I3C0 + 0x88))
#define I3C_MINTSET (*(volatile uint32_t *)(I3C0 + 0x90))

#define MSTATUS_MCTRLDONE 0x200u
#define MSTATUS_COMPLETE  0x400u
/* MCTRL: REQUEST=1 (EmitStartAddr) with a dummy address/dir; any nonzero
 * REQUEST field starts a controller operation. */
#define MCTRL_REQUEST_EMIT_START 0x1u

#define NVIC_ISER2 (*(volatile uint32_t *)0xE000E108u)  /* IRQ 64..95 */
#define I3C0_IRQ 95

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

void i3c0_handler(void)
{
    if (I3C_MSTATUS & MSTATUS_COMPLETE) {
        irq_count++;
        I3C_MSTATUS = MSTATUS_COMPLETE | MSTATUS_MCTRLDONE;   /* W1C */
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("I3C test\r\n");

    I3C_MINTSET = MSTATUS_COMPLETE | MSTATUS_MCTRLDONE;
    NVIC_ISER2 = (1u << (I3C0_IRQ - 64));
    __asm__ volatile ("cpsie i");

    /* Issue a controller request -> message completes -> interrupt. */
    I3C_MCTRL = MCTRL_REQUEST_EMIT_START;

    while (irq_count < 1) {
    }

    puts_(irq_count >= 1 ? "I3C PASS\r\n" : "I3C FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[120] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + I3C0_IRQ] = i3c0_handler,  /* exception 111 = IRQ 95 */
};
