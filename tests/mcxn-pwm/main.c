/*
 * MCXN947 eFlexPWM reload + NVIC interrupt test.
 *
 * Configures PWM0 submodule 0 (INIT=0, VAL1=modulo), enables the reload
 * interrupt (INTEN.RIE) and the PWM0 submodule-0 NVIC line (IRQ 114), then
 * starts the submodule (MCTRL.RUN bit0).  The model runs a periodic reload
 * timer at the nominal counter rate; each reload sets STS.RF and raises the
 * interrupt.  The ISR clears RF and counts; after several reloads the test
 * stops the submodule.  Prints "PWM PASS" once multiple reload interrupts have
 * been delivered through the counter -> STS.RF -> NVIC -> handler path.
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

#define PWM0 0x400CE000u
#define SM0_INIT  (*(volatile uint16_t *)(PWM0 + 0x02))
#define SM0_VAL1  (*(volatile uint16_t *)(PWM0 + 0x0E))
#define SM0_STS   (*(volatile uint16_t *)(PWM0 + 0x24))
#define SM0_INTEN (*(volatile uint16_t *)(PWM0 + 0x26))
#define PWM_MCTRL (*(volatile uint16_t *)(PWM0 + 0x188))

#define STS_RF    0x1000u
#define INTEN_RIE 0x1000u
#define MCTRL_RUN_SM0 0x0100u

#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)  /* IRQ 96..127 */
#define PWM0_IRQ 114

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

static volatile uint32_t reloads;

void pwm0_handler(void)
{
    if (SM0_STS & STS_RF) {
        reloads++;
        SM0_STS = STS_RF;   /* write-1-to-clear the reload flag */
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("PWM test\r\n");

    SM0_INIT = 0;
    SM0_VAL1 = 0x1000;        /* modulo -> nominal period */
    SM0_INTEN = INTEN_RIE;    /* reload interrupt enable */

    NVIC_ISER3 = (1u << (PWM0_IRQ - 96));
    __asm__ volatile ("cpsie i");

    PWM_MCTRL = MCTRL_RUN_SM0;   /* start submodule 0 -> periodic reloads */

    while (reloads < 3) {
    }

    PWM_MCTRL = 0;            /* stop the submodule */

    puts_(reloads >= 3 ? "PWM PASS\r\n" : "PWM FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[140] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + PWM0_IRQ] = pwm0_handler,  /* exception 130 = IRQ 114 */
};
