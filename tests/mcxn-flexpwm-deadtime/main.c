/*
 * MCXN947 eFlexPWM complementary pair + DEAD-TIME INSERTION.
 *
 * In complementary mode (CTRL2[INDEP]=0, the reset default) PWM_B is the complement of PWM_A,
 * and DEAD-TIME is inserted on each leading edge: PWM_A's 0->1 edge (at VAL2) is delayed by
 * DTCNT0, PWM_B's (at VAL3) by DTCNT1.  During each delay BOTH outputs are forced inactive --
 * the DEAD BAND that stops shoot-through through both transistors of an inverter leg.  Zero
 * dead-time shorts the DC bus, which is why DTCNT resets to 0x07FF, not 0.
 *
 * Until now DTCNT carried its correct reset value but had NO WAVEFORM EFFECT.  This proves the
 * insertion: the counter is held STATIC at INIT (submodule not RUN), so the output at a chosen
 * position is deterministic, and the harness reads pwm-a-output / pwm-b-output over QMP for each.
 *
 * VAL2=0x4000, VAL3=0x8000, PRSC=0 so DTCNT is in counts directly; DTCNT0=DTCNT1=0x1000.
 * A's commanded region is [VAL2,VAL3); with dead-time A is high only in [VAL2+0x1000, VAL3):
 *   deadlead   INIT=0x4800  in [VAL2, VAL2+DT)      -> A=0 B=0  (dead band, A leading edge)
 *   aactive    INIT=0x6000  in [VAL2+DT, VAL3)      -> A=1 B=0
 *   deadtrail  INIT=0x8800  in [VAL3, VAL3+DT)      -> A=0 B=0  (dead band, B leading edge)
 *   bactive    INIT=0xA000  in [VAL3+DT, VAL1]      -> A=0 B=1
 * And the DIRECT discriminator -- same INIT=VAL2, dead-time toggled:
 *   withdead   INIT=0x4000, DTCNT0=0x1000           -> A=0 B=0  (dead band swallows VAL2)
 *   nodead     INIT=0x4000, DTCNT0=0                -> A=1 B=0  (no dead-time: A on at VAL2)
 * A model that ignores DTCNT reports A=1 for BOTH withdead and nodead -> FAIL.
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
#define PWM_SM0_INIT   (*(volatile uint16_t *)(PWM0 + 0x02))
#define PWM_SM0_CTRL2  (*(volatile uint16_t *)(PWM0 + 0x04))
#define PWM_SM0_CTRL   (*(volatile uint16_t *)(PWM0 + 0x06))
#define PWM_SM0_VAL1   (*(volatile uint16_t *)(PWM0 + 0x0E))
#define PWM_SM0_VAL2   (*(volatile uint16_t *)(PWM0 + 0x12))
#define PWM_SM0_VAL3   (*(volatile uint16_t *)(PWM0 + 0x16))
#define PWM_SM0_DTCNT0 (*(volatile uint16_t *)(PWM0 + 0x30))
#define PWM_SM0_DTCNT1 (*(volatile uint16_t *)(PWM0 + 0x32))
#define PWM_OUTEN      (*(volatile uint16_t *)(PWM0 + 0x180))

#define OUTEN_AB   0x0110u   /* PWMA_EN(SM0)=bit8 | PWMB_EN(SM0)=bit4 */
#define DT         0x1000u

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
    puts_("DEADTIME test\r\n");

    PWM_SM0_CTRL2 = 0;           /* INDEP=0 -> complementary pair (also the reset default) */
    PWM_SM0_CTRL  = 0;           /* PRSC=0 -> DTCNT in counts */
    PWM_SM0_VAL1  = 0xE000;      /* period */
    PWM_SM0_VAL2  = 0x4000;      /* PWM_A on  */
    PWM_SM0_VAL3  = 0x8000;      /* PWM_A off */
    PWM_SM0_DTCNT0 = DT;
    PWM_SM0_DTCNT1 = DT;
    PWM_OUTEN = OUTEN_AB;

    /* Each: set the static counter position (INIT), print marker + expected A,B, hold for read. */
    PWM_SM0_INIT = 0x4800; puts_("DT deadlead 0 0\r\n");  hold();  /* dead band, A leading edge */
    PWM_SM0_INIT = 0x6000; puts_("DT aactive 1 0\r\n");   hold();  /* A driving              */
    PWM_SM0_INIT = 0x8800; puts_("DT deadtrail 0 0\r\n");  hold(); /* dead band, B leading edge */
    PWM_SM0_INIT = 0xA000; puts_("DT bactive 0 1\r\n");   hold();  /* B driving              */

    /* Direct dead-time discriminator at INIT = VAL2. */
    PWM_SM0_INIT = 0x4000;
    PWM_SM0_DTCNT0 = DT;   puts_("DT withdead 0 0\r\n"); hold();   /* dead band swallows VAL2 */
    PWM_SM0_DTCNT0 = 0;    puts_("DT nodead 1 0\r\n");   hold();   /* no dead-time: A on at VAL2 */

    puts_("DT-DONE\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
