/*
 * MCXN947 eFlexPWM PWM_X auxiliary output.
 *
 * PWM_X is the submodule's third (auxiliary) output, generated independently of the A/B pair:
 * SET at VAL0 (effective at VAL0+1), RESET at VAL1, so it is high in (VAL0, VAL1).  OCTRL[POLX]
 * inverts it, OUTEN[PWMX_EN] gates it, and a latched fault DISMAP-mapped to PWM_X forces it OFF.
 * The pin has no on-chip consumer in emulation, so it is operator-observable via the read-only
 * "pwm-x-output" QOM property; the static counter (submodule not RUN) makes the compare
 * deterministic.
 *
 * VAL0=0x3000, VAL1=0xC000 -> PWM_X high in (0x3000, 0xC000):
 *   xhigh   INIT=0x6000 (in range)          -> X=1
 *   xlow    INIT=0x1000 (below VAL0)         -> X=0   (compare depends on the counter)
 *   xpol    INIT=0x6000, OCTRL[POLX]=1       -> X=0   (polarity inverts)
 *   xouten  INIT=0x6000, PWMX_EN=0           -> X=0   (pin not driven)
 *   xfault  INIT=0x6000, latched fault       -> X=0   (safety force-off via DISMAP DIS0X)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP_CTRL (*(volatile uint32_t *)0x400B4018u)
#define LP_STAT (*(volatile uint32_t *)0x400B4014u)
#define LP_DATA (*(volatile uint32_t *)0x400B401Cu)
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

#define PWM0        0x400CE000u
#define PWM_SM0_INIT  (*(volatile uint16_t *)(PWM0 + 0x02))
#define PWM_SM0_VAL0  (*(volatile uint16_t *)(PWM0 + 0x0A))
#define PWM_SM0_VAL1  (*(volatile uint16_t *)(PWM0 + 0x0E))
#define PWM_SM0_OCTRL (*(volatile uint16_t *)(PWM0 + 0x22))
#define PWM_OUTEN     (*(volatile uint16_t *)(PWM0 + 0x180))
#define PWM_FCTRL     (*(volatile uint16_t *)(PWM0 + 0x18C))

#define OCTRL_POLX   0x0100u
#define OUTEN_PWMX   0x0001u
#define FCTRL_FLVL0  0x1000u

static void hold(void)
{
    volatile int g;
    for (g = 0; g < 6000000; g++) {
        (void)PWM_OUTEN;
    }
}

void cpu0_main(void)
{
    LP_CTRL = (1u << 19);
    puts_("PWMX test\r\n");

    PWM_SM0_VAL0 = 0x3000;
    PWM_SM0_VAL1 = 0xC000;
    PWM_SM0_OCTRL = 0;
    PWM_OUTEN = OUTEN_PWMX;

    PWM_SM0_INIT = 0x6000; puts_("PWMX xhigh 1\r\n"); hold();   /* in (VAL0,VAL1) */
    PWM_SM0_INIT = 0x1000; puts_("PWMX xlow 0\r\n");  hold();   /* below VAL0     */

    PWM_SM0_INIT = 0x6000;
    PWM_SM0_OCTRL = OCTRL_POLX; puts_("PWMX xpol 0\r\n"); hold();   /* POLX inverts */

    PWM_SM0_OCTRL = 0; PWM_OUTEN = 0; puts_("PWMX xouten 0\r\n"); hold();  /* disabled */

    /* Fault force-off: enable PWMX, arm an active-high fault, harness drives it. */
    PWM_OUTEN = OUTEN_PWMX;
    PWM_FCTRL = FCTRL_FLVL0;
    puts_("PWMX-FAULT-SET\r\n"); hold();          /* harness drives fault-input = 1 -> FFLAG latch */
    puts_("PWMX xfault 0\r\n"); hold();

    puts_("PWMX-FAULT-CLR\r\n"); hold();           /* harness drives fault-input = 0 */
    puts_("PWMX-DONE\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
