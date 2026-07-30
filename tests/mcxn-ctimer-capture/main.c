/*
 * CTIMER INPUT CAPTURE -- timestamp an external event by latching the running counter.
 *
 * A capture input (a CTIMER pin, selected by INPUTMUX's CTIMERnCAPm mux) with a CCR-chosen
 * edge LATCHES the live timer counter TC into CR0, and -- if CCR[CAP0I] -- raises the timer
 * interrupt.  This is how firmware measures a pulse width or timestamps an event with no
 * polling.  It was DEAD: CR0..3 were read-only storage nothing ever loaded, so a capture
 * driver read a frozen zero.
 *
 * The capture pin has no signal source in emulation, so the operator drives it over QMP
 * ("capture-input" QOM property), exactly like the CMP output / FlexPWM capture input.
 *
 *   PHASE A (CCR = CAP0RE | CAP0I): a RISING edge latches TC into CR0 and sets IR[CR0INT].
 *   PHASE B (still CAP0RE only): a FALLING edge must NOT capture -- CR0 unchanged, no new
 *           flag -- because only the rising edge is enabled (the edge-select gate).
 *
 * The CPU never writes CR0 (read-only) and never software-triggers the capture.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4 + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

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

static void puthex(uint32_t v)
{
    const char *H = "0123456789ABCDEF";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        putc_(H[(v >> i) & 0xF]);
    }
}

/* ---- clocks (CTIMER0 <- FRO_HF) ------------------------------------------ */
#define SCG0        0x40044000u
#define SCG_FIRCCSR (*(volatile uint32_t *)(SCG0 + 0x300))
#define FIRCCSR_FIRCEN (1u << 0)
#define CTIMER0CLKSEL (*(volatile uint32_t *)0x4000026Cu)
#define CTIMER0CLKDIV (*(volatile uint32_t *)0x400003D0u)
#define SEL_FROHF   3u

/* ---- CTIMER0 (base 0x4000C000) ------------------------------------------- */
#define CT0 0x4000C000u
#define CT0_IR  (*(volatile uint32_t *)(CT0 + 0x00))
#define CT0_TCR (*(volatile uint32_t *)(CT0 + 0x04))
#define CT0_TC  (*(volatile uint32_t *)(CT0 + 0x08))
#define CT0_CCR (*(volatile uint32_t *)(CT0 + 0x28))
#define CT0_CR0 (*(volatile uint32_t *)(CT0 + 0x2C))
#define TCR_RUN 1u
#define TCR_RST 2u
#define CCR_CAP0RE (1u << 0)
#define CCR_CAP0FE (1u << 1)
#define CCR_CAP0I  (1u << 2)
#define IR_CR0INT  (1u << 4)

static void spin(int n)
{
    volatile int d;

    for (d = 0; d < n; d++) {
    }
}

void cpu0_main(void)
{
    int ok = 1;
    uint32_t cr0a, d;

    LP_CTRL = CTRL_TE;
    puts_("CTIMER-CAPTURE test\r\n");

    SCG_FIRCCSR   = SCG_FIRCCSR | FIRCCSR_FIRCEN;
    CTIMER0CLKDIV = 0;
    CTIMER0CLKSEL = SEL_FROHF;

    /* Free-running CTIMER0 (no match action -> counts up); capture ch0 on RISING edge. */
    CT0_CCR = CCR_CAP0RE | CCR_CAP0I;
    CT0_TCR = TCR_RST;
    CT0_TCR = TCR_RUN;

    /* PHASE A -- the operator drives a RISING edge; wait for the capture interrupt. */
    puts_("CTIMER-CAPTURE ARMED\r\n");
    for (d = 0; d < 80000000 && !(CT0_IR & IR_CR0INT); d++) {
    }
    cr0a = CT0_CR0;
    puts_("  phaseA CR0="); puthex(cr0a);
    puts_(" TC="); puthex(CT0_TC); puts_("\r\n");
    ok &= !!(CT0_IR & IR_CR0INT);        /* capture raised the interrupt flag */
    ok &= (cr0a != 0);                   /* CR0 latched a live (non-reset) count */
    puts_(ok ? "  phaseA: captured on rising edge (correct)\r\n"
             : "  phaseA: FAILED\r\n");

    /* PHASE B -- clear the flag, then the operator drives a FALLING edge.  With only
     * CAP0RE enabled it must NOT capture: CR0 stays put and no new flag appears. */
    CT0_IR = IR_CR0INT;                  /* W1C */
    puts_("CTIMER-CAPTURE FALLING\r\n");
    spin(20000000);                      /* operator injects the falling edge here */
    puts_("  phaseB CR0="); puthex(CT0_CR0);
    puts_(" IR="); puthex(CT0_IR); puts_("\r\n");
    if ((CT0_CR0 != cr0a) || (CT0_IR & IR_CR0INT)) {
        puts_("  FAIL: falling edge captured with only CAP0RE enabled\r\n");
        ok = 0;
    } else {
        puts_("  phaseB: falling edge ignored (edge-select correct)\r\n");
    }
    CT0_TCR = 0;

    puts_(ok ? "CTIMER-CAPTURE PASS\r\n" : "CTIMER-CAPTURE FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
