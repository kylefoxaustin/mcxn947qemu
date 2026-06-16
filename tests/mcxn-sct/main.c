/*
 * MCXN947 SCT (SCTimer/PWM) match-event + NVIC interrupt test.
 *
 * Programs the match/limit reload (MATCHREL0), enables the event-0 interrupt
 * (EVEN bit0) and the SCT0 NVIC line (IRQ 33), then starts the counter by
 * writing CTRL (clearing HALT/STOP).  The model runs a periodic event timer at
 * the nominal counter rate; each period sets EVFLAG bit0 and raises the
 * interrupt.  The ISR clears the flag and counts; after several events the test
 * halts the counter.  Prints "SCT PASS" once multiple match-event interrupts
 * have been delivered through the counter -> EVFLAG -> NVIC -> handler path.
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

#define SCT0 0x40091000u
#define SCT_CTRL     (*(volatile uint32_t *)(SCT0 + 0x004))
#define SCT_EVEN     (*(volatile uint32_t *)(SCT0 + 0x0F0))
#define SCT_EVFLAG   (*(volatile uint32_t *)(SCT0 + 0x0F4))
#define SCT_MATCHREL0 (*(volatile uint32_t *)(SCT0 + 0x180))

#define CTRL_CLRCTR_L (1u << 3)
#define CTRL_HALT_L   (1u << 2)
#define EV0           (1u << 0)

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define SCT0_IRQ 33

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

static volatile uint32_t events;

void sct0_handler(void)
{
    if (SCT_EVFLAG & EV0) {
        events++;
        SCT_EVFLAG = EV0;   /* write-1-to-clear */
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("SCT test\r\n");

    SCT_MATCHREL0 = 0x1000;   /* match/limit -> nominal period */
    SCT_EVEN = EV0;           /* enable event-0 interrupt */

    NVIC_ISER1 = (1u << (SCT0_IRQ - 32));
    __asm__ volatile ("cpsie i");

    SCT_CTRL = CTRL_CLRCTR_L; /* clear counter + run (HALT/STOP clear) */

    while (events < 3) {
    }

    SCT_CTRL = CTRL_HALT_L;   /* halt the counter */

    puts_(events >= 3 ? "SCT PASS\r\n" : "SCT FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[60] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + SCT0_IRQ] = sct0_handler,  /* exception 49 = IRQ 33 */
};
