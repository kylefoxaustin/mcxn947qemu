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
#define SM0_CTRL  (*(volatile uint16_t *)(PWM0 + 0x06))   /* PRSC lives here */
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

/*
 * Predicted from CONFIGURATION AND THE RM — and this time that claim is true.
 *
 * ⚠ THE OLD VERSION OF THIS COMMENT SAID "nothing below comes from the model",
 * AND IT WAS A LIE.  `PWM_TICK_NS 10` was the MODEL'S OWN INVENTED CONSTANT — a
 * ~100 MHz tick appearing nowhere in the RM — so the prediction and the model
 * divided by THE SAME MADE-UP NUMBER.  SysTick independently measures ELAPSED
 * TIME and therefore genuinely catches a DRIFTING carrier (which is what it was
 * written for, and it did).  But it CANNOT catch a wrong TICK RATE: both sides
 * are wrong together and SysTick confirms them.  A careful measurement on one
 * side of a comparison made the other side INVISIBLE.
 *
 * ⚠ AND THE TEST NEVER SET CTRL[PRSC], SO IT NEVER NOTICED THAT THE MODEL
 * IGNORED IT ENTIRELY.  PRSC is a 3-bit divide-by-2^PRSC (1..128) and eFlexPWM
 * carrier frequency IS motor control: a driver asking for an 8x slower carrier
 * got the same one, silently, and the emulator agreed with itself.  One shape,
 * one green, one invisible bug — the same degenerate-oracle failure as testing a
 * matrix engine only on square matrices.
 *
 * Per the RM the counter runs from the IPBus clock divided by 2^PRSC.  The SoC
 * drives sysclk at 150 MHz and SysTick runs off the processor clock, so:
 *
 *     SysTick ticks per PWM period  ==  (VAL1 - INIT + 1) << PRSC     (exactly)
 *
 * The counts below therefore come from the RM's semantics, not from the model.
 */
#define PWM_VAL1         0x1000
#define PERIODS_MEASURED 8
#define EXPECTED_TICKS_FOR(prsc) \
    (((uint32_t)(PWM_VAL1 + 1) << (prsc)) * PERIODS_MEASURED)

/* SM_CTRL[PRSC] (CMSIS PWM_CTRL_PRSC_*): divide the counter clock by 2^PRSC. */
#define CTRL_PRSC(n)     ((uint16_t)((n) << 4))

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

/* Bring the core/SysTick clock to 150 MHz via PLL0, as BOARD_InitBootClocks does
 * (the SCG boots on FRO_HF at 48 MHz; timing measured vs SysTick needs the 150 MHz
 * operating point this test's goldens assume). */
static void clock_init_150m(void)
{
    volatile uint32_t *scg = (volatile uint32_t *)0x40044000u;
    scg[0x300 / 4] |= 1u;             /* FIRCCSR |= FIRCEN           */
    scg[0x504 / 4]  = 0x020035B0u;    /* APLLCTRL: SOURCE=1 (Clk48M) */
    scg[0x50C / 4]  = 8u;             /* APLLNDIV N=8                */
    scg[0x510 / 4]  = 50u;            /* APLLMDIV M=50               */
    scg[0x514 / 4]  = 1u;             /* APLLPDIV P=1                */
    scg[0x500 / 4] |= 3u;             /* APLLCSR PWREN|CLKEN         */
    scg[0x014 / 4]  = (5u << 24);     /* RCCR SCS = PLL0 -> 150 MHz  */
}

void cpu0_main(void)
{
    uint32_t elapsed;
    int ok = 1;
    int p;

    LP_CTRL = CTRL_TE;
    clock_init_150m();
    puts_("PWM test\r\n");

    /* Independent time base: SysTick free-running off the processor clock. */
    SYST_RVR = SYST_MASK;
    SYST_CVR = 0;
    SYST_CSR = SYST_ENABLE | SYST_CLKSOURCE;

    NVIC_ISER3 = (1u << (PWM0_IRQ - 96));

    /*
     * SWEEP THE PRESCALER.  A single shape cannot catch a prescaler that is not
     * modelled — every PRSC must move the carrier by exactly 2^PRSC, or the
     * dimension is untested.  PRSC=0 (/1), 1 (/2), 3 (/8): if the model ignores
     * PRSC, the /2 and /8 rows come back at the /1 period and FAIL here.
     */
    for (p = 0; p < 3; p++) {
        static const uint8_t prsc_list[3] = { 0, 1, 3 };
        uint8_t prsc = prsc_list[p];
        uint32_t expect = EXPECTED_TICKS_FOR(prsc);
        uint32_t diff;

        reloads = 0;
        SM0_INIT = 0;
        SM0_VAL1 = PWM_VAL1;              /* modulo -> carrier period      */
        SM0_CTRL = CTRL_PRSC(prsc);       /* counter clock / 2^PRSC        */
        SM0_INTEN = INTEN_RIE;            /* reload interrupt enable       */

        __asm__ volatile ("cpsie i");
        PWM_MCTRL = MCTRL_RUN_SM0;        /* start submodule 0             */

        while (reloads < 1 + PERIODS_MEASURED) {
        }

        PWM_MCTRL = 0;                    /* stop the submodule            */
        __asm__ volatile ("cpsid i");

        /* SysTick counts DOWN; modular arithmetic absorbs the wrap. */
        elapsed = (t_first - t_last) & SYST_MASK;

        diff = (elapsed > expect) ? (elapsed - expect) : (expect - elapsed);

        puts_("  PRSC="); putdec(prsc);
        puts_(" measured "); putdec(elapsed);
        puts_(" SysTick ticks, expected "); putdec(expect);
        puts_("\r\n");

        /* Within 0.1%: the carrier must scale EXACTLY with 2^PRSC. */
        ok &= (diff * 1000 <= expect);
    }

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
