/*
 * MCXN947 eFlexPWM output waveform + the dangerous-zero reset values.
 *
 * TWO things this proves:
 *
 * 1. RESET VALUES the RM gives as NON-zero, where zero is a legal, meaningful, CATASTROPHIC
 *    value.  DTCNT0/DTCNT1 (dead-time counters) reset to 0x07FF -- zero dead-time is a direct
 *    short across the DC bus through both transistors of an inverter leg.  DISMAP0 resets to
 *    0xFFFF (every fault disables every output -- the safe default).  The model memset(0) shipped
 *    both zeros; these are now the RM values, and are GUEST-readable, so the guest asserts them.
 *
 * 2. The PWM_A OUTPUT WAVEFORM, which did not exist: the output flip-flop is SET at VAL2 and
 *    RESET at VAL3, so the counter position determines the level; OCTRL[POLA] inverts it,
 *    OUTEN[PWMA_EN] gates it, and a latched FAULT that DISMAP maps to PWM_A forces it OFF (the
 *    safety cut).  The output pin has no on-chip consumer in emulation, so it is OPERATOR-
 *    OBSERVABLE via the read-only "pwm-a-output" QOM property (a scope on the pin); the harness
 *    reads it for each config the guest sets.  The counter is held STATIC at INIT (the submodule
 *    is never RUN), so the compare is deterministic -- no racing the carrier.
 *
 * The guest configures each scenario, prints "PWMOUT <tag> <expected>", and HOLDS it (a bounded
 * volatile spin) long enough for the harness to sample pwm-a-output; a couple of FAULT markers
 * ask the harness to drive/clear the fault-input pin.  The harness decides PASS/FAIL (it holds
 * the output samples) combined with the guest's reset-value verdict.
 *
 * ⚠ Scope, stated: PWM_A of submodule 0; dead-time INSERTION (DTCNT shifting the A/B edges) is a
 * named seam -- DTCNT now carries its real reset value and is readable, but the sub-count edge
 * delay it represents is not inserted into the modelled level.
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
#define PWM_SM0_VAL2  (*(volatile uint16_t *)(PWM0 + 0x12))
#define PWM_SM0_VAL3  (*(volatile uint16_t *)(PWM0 + 0x16))
#define PWM_SM0_OCTRL (*(volatile uint16_t *)(PWM0 + 0x22))
#define PWM_SM0_DISMAP0 (*(volatile uint16_t *)(PWM0 + 0x2C))
#define PWM_SM0_DTCNT0 (*(volatile uint16_t *)(PWM0 + 0x30))
#define PWM_SM0_DTCNT1 (*(volatile uint16_t *)(PWM0 + 0x32))
#define PWM_OUTEN     (*(volatile uint16_t *)(PWM0 + 0x180))
#define PWM_FCTRL     (*(volatile uint16_t *)(PWM0 + 0x18C))
#define PWM_FSTS      (*(volatile uint16_t *)(PWM0 + 0x18E))

#define OCTRL_POLA    0x0400u
#define OUTEN_PWMA0   0x0100u
#define FCTRL_FLVL0   0x1000u

/* Hold the current config long enough for the harness to sample pwm-a-output over QMP.
 * The spin reads a volatile MMIO reg so -O2 cannot delete it. */
static void hold(void)
{
    volatile int g;
    for (g = 0; g < 6000000; g++) {          /* ~hundreds of ms: covers the QMP sample latency */
        (void)PWM_FSTS;
    }
}

void cpu0_main(void)
{
    int rv;

    LP_CTRL = (1u << 19);
    puts_("PWMOUT test\r\n");

    /* --- the dangerous-zero reset values (guest-readable) --- */
    rv = (PWM_SM0_DTCNT0 == 0x07FFu) && (PWM_SM0_DTCNT1 == 0x07FFu) &&
         (PWM_SM0_DISMAP0 == 0xFFFFu);
    puts_(rv ? "RESETVALS ok=1\r\n" : "RESETVALS ok=0\r\n");

    /* Static counter at INIT (submodule never RUN): the compare is deterministic. */
    PWM_SM0_INIT = 0x1000;
    PWM_SM0_OCTRL = 0;

    /* 1: INIT bracketed by [VAL2,VAL3) -> HIGH. */
    PWM_SM0_VAL2 = 0x0800; PWM_SM0_VAL3 = 0x2000;
    PWM_OUTEN = OUTEN_PWMA0;
    puts_("PWMOUT bracket 1\r\n"); hold();

    /* 2: INIT below VAL2 -> LOW (the compare actually depends on the counter). */
    PWM_SM0_VAL2 = 0x1800;
    puts_("PWMOUT below 0\r\n"); hold();

    /* 3: bracketed again but OCTRL[POLA] inverts -> LOW. */
    PWM_SM0_VAL2 = 0x0800; PWM_SM0_OCTRL = OCTRL_POLA;
    puts_("PWMOUT pola 0\r\n"); hold();

    /* 4: OUTEN cleared -> the PWM does not drive the pin -> LOW despite the compare. */
    PWM_SM0_OCTRL = 0; PWM_OUTEN = 0;
    puts_("PWMOUT outen 0\r\n"); hold();

    /* 5: re-enable + a latched FAULT mapped to PWM_A (DISMAP reset 0xFFFF) -> forced OFF. */
    PWM_OUTEN = OUTEN_PWMA0;
    PWM_FCTRL = FCTRL_FLVL0;                 /* active-high fault, no IRQ needed */
    puts_("PWMOUT-FAULT-SET\r\n"); hold();    /* harness drives fault-input = 1 -> FFLAG latches */
    puts_("PWMOUT faulted 0\r\n"); hold();

    /* 6: clear the fault->PWM_A mapping bit (DISMAP DIS0A[0]); the fault stays latched but no
     *    longer disables PWM_A -> the output returns HIGH.  Proves the DISMAP mapping is real. */
    PWM_SM0_DISMAP0 = 0xFFFEu;
    puts_("PWMOUT unmapped 1\r\n"); hold();

    puts_("PWMOUT-FAULT-CLR\r\n"); hold();    /* harness drives fault-input = 0 */
    puts_("PWMOUT-DONE\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
