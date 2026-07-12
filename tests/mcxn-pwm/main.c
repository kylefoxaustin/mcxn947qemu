/*
 * MCXN947 eFlexPWM test — the carrier PERIOD is measured, not just counted.
 *
 * The old version asserted `reloads >= 3` and nothing else.  That cannot fail in
 * the dimension it exists to protect: mutation-testing showed a carrier period
 * THREE TIMES too long still passed, because an interrupt still fired — just at
 * the wrong rate.  On a motor-control part that is not a cosmetic gap.  The PWM
 * carrier frequency is what an entire FOC loop is built on, and a silently-3x-wrong
 * carrier is precisely the class of silent-wrong this project exists to kill.
 *
 * "An IRQ fired" is not a golden.  Neither is a range: 3x a plausible value is
 * still plausible.  (rt1180emulator, who found the identical hole across his whole
 * FOC path when he ran the same mutation sweep: "A RANGE IS NOT A GOLDEN.")
 *
 * So: PREDICT the period from configuration, MEASURE it against an INDEPENDENT
 * clock.
 *
 *   predicted   period = (VAL1 - INIT + 1) * PWM tick = (0x1000 + 1) * 10 ns
 *                       = 40970 ns
 *               SysTick runs at SYSCLK = 150 MHz, so one carrier is
 *                       40970e-9 * 150e6 = 6145.5 SysTick ticks.
 *
 *   measured    SysTick — the Arm CORE timer.  It is part of the CPU, not one of
 *               this machine's peripheral models, so a bug in the eFlexPWM model
 *               cannot make it wrong in the same way.  That independence is the
 *               whole point: timing the reloads against the PWM's own notion of
 *               time would only restate the model to itself.
 *
 * Prints "PWM PASS" only if the reloads arrive AND arrive at the right rate.
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

/* SysTick: an Arm CORE timer, independent of every peripheral model under test. */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)
#define SYST_ENABLE    (1u << 0)
#define SYST_CLKSOURCE (1u << 2)   /* 1 = processor clock (SYSCLK) */
#define SYST_MASK  0x00FFFFFFu     /* 24-bit down-counter */

/* Predicted from configuration alone — nothing below comes from the model. */
#define PWM_VAL1         0x1000
#define PWM_TICK_NS      10        /* eFlexPWM counter tick */
#define SYSCLK_MHZ       150
#define PERIODS_MEASURED 8
#define EXPECTED_TICKS  (((PWM_VAL1 + 1) * PWM_TICK_NS * SYSCLK_MHZ * \
                          PERIODS_MEASURED) / 1000)

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

static void putdec(uint32_t v)
{
    char b[12];
    int i = 0;

    if (!v) {
        putc_('0');
        return;
    }
    while (v) {
        b[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i--) {
        putc_(b[i]);
    }
}

static volatile uint32_t reloads;
static volatile uint32_t t_first, t_last;

void pwm0_handler(void)
{
    SM0_STS = STS_RF;                 /* W1C the reload flag */
    reloads++;
    if (reloads == 1) {
        t_first = SYST_CVR;           /* start of the measured window */
    } else if (reloads == 1 + PERIODS_MEASURED) {
        t_last = SYST_CVR;            /* PERIODS_MEASURED carriers later */
    }
}

void cpu0_main(void)
{
    uint32_t elapsed, lo, hi;
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("PWM test\r\n");

    /* Independent time base: SysTick free-running off the processor clock. */
    SYST_RVR = SYST_MASK;
    SYST_CVR = 0;
    SYST_CSR = SYST_ENABLE | SYST_CLKSOURCE;

    SM0_INIT = 0;
    SM0_VAL1 = PWM_VAL1;      /* modulo -> carrier period */
    SM0_INTEN = INTEN_RIE;    /* reload interrupt enable */

    NVIC_ISER3 = (1u << (PWM0_IRQ - 96));
    __asm__ volatile ("cpsie i");

    PWM_MCTRL = MCTRL_RUN_SM0;   /* start submodule 0 -> periodic reloads */

    while (reloads < 1 + PERIODS_MEASURED) {
    }

    PWM_MCTRL = 0;            /* stop the submodule */
    __asm__ volatile ("cpsid i");

    /* SysTick counts DOWN; modular arithmetic absorbs the wrap. */
    elapsed = (t_first - t_last) & SYST_MASK;

    puts_("  "); putdec(PERIODS_MEASURED); puts_(" carriers = ");
    putdec(elapsed); puts_(" SysTick ticks, expected ");
    putdec(EXPECTED_TICKS); puts_("\r\n");

    /*
     * +/-1%.  The measurement is EXACT under -icount (which run.sh passes):
     * virtual time is then derived from instructions retired, not from host wall
     * time, so it is deterministic and immune to load on the build box.  Without
     * icount this same measurement swings +/-13% run to run — a flaky "golden" is
     * not a golden, it is a coin toss with a reference value printed next to it.
     */
    lo = EXPECTED_TICKS - EXPECTED_TICKS / 100;
    hi = EXPECTED_TICKS + EXPECTED_TICKS / 100;
    ok &= (elapsed >= lo && elapsed <= hi);
    ok &= (reloads >= 1 + PERIODS_MEASURED);

    puts_(ok ? "PWM PASS\r\n" : "PWM FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[131] = {
    [0]  = (vec_t)0x20010000u,      /* initial MSP */
    [1]  = cpu0_main,               /* Reset_Handler */
    [16 + PWM0_IRQ] = pwm0_handler,
};
