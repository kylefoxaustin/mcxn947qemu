/*
 * MCXN947 eFlexPWM FAULT protection + NVIC interrupt (operator-driven).
 *
 * A FAULT input is the safety path that trips motor gate-drive on silicon.  The pin has no
 * signal source in emulation, so FAULT0's level is OPERATOR-DRIVEN via the "fault-input" QOM
 * property (the same seam as capture-a-input / CMP's comparator-output / PINT's pin-input).
 * Before this was wired, the fault registers were inert: a FAULT assertion set nothing, raised
 * no interrupt, and a driver's fault handler never ran -- a silent hole in the safety path.
 *
 * Three mutation-proven axes, each POSITIVE-SIGNAL (a SET flag or a counted IRQ proves delivery
 * -- no timed "check absence", which -O2 optimizes into a flaky crutch):
 *
 *   A  DETECTION + FIE-GATED INTERRUPT.  FLVL0=1 (active high), FIE0=1: assert the fault ->
 *      FSTS[FFLAG0] latches, FSTS[FFPIN0] mirrors the live pin, and the FLEXPWM0_FAULT IRQ
 *      (113) is taken exactly once.  Also the CLEAR INTERLOCK: a W1C to FFLAG0 while the fault
 *      is STILL ASSERTED is refused (a live fault cannot be cleared -- the safety latch); once
 *      the operator deasserts, the W1C clears it.
 *   B  FIE GATES THE INTERRUPT.  FIE0=0 with NVIC 113 still enabled: assert -> FFLAG0 still
 *      latches (detection is independent of FIE) but NO interrupt fires.  FFLAG0 SET is the
 *      delivery proof; the unchanged IRQ count proves FIE gated it (not NVIC masking).
 *   C  FLVL POLARITY.  FLVL0=0 (active LOW): a HIGH input is NOT a fault.  Drive HIGH ->
 *      FFPIN0 SET (raw-pin mirror = delivery proof) but FFLAG0 CLEAR (high isn't a fault when
 *      active-low); then drive LOW -> FFLAG0 latches + IRQ.  A model ignoring FLVL faults on
 *      the HIGH -> FFLAG0 set early -> FAIL.
 *
 * ⚠ Scope, stated: the fault's PRIMARY silicon effect -- forcing the mapped PWM OUTPUTS to
 * their safe state (FCTRL[FSAFE]/DISMAP) -- is NOT modelled, because this model has no output-
 * pin / waveform representation (OUTEN/MASK are inert).  What is proven is the fault DETECTION
 * + STATUS + INTERRUPT path a fault handler binds to; the output-force-off is a named seam.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP_CTRL (*(volatile uint32_t *)0x400B4018u)
#define LP_STAT (*(volatile uint32_t *)0x400B4014u)
#define LP_DATA (*(volatile uint32_t *)0x400B401Cu)
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void putdec(uint32_t v) { char b[12]; int i = 0; if (!v) { putc_('0'); return; }
    while (v) { b[i++] = (char)('0' + v % 10); v /= 10; } while (i--) putc_(b[i]); }

/* ---- eFlexPWM0 fault registers (shared top-level block) ------------------- */
#define PWM0        0x400CE000u
#define PWM_FCTRL   (*(volatile uint16_t *)(PWM0 + 0x18C))
#define PWM_FSTS    (*(volatile uint16_t *)(PWM0 + 0x18E))

#define FCTRL_FIE0   0x0001u    /* Fault Interrupt Enable, fault 0            */
#define FCTRL_FLVL0  0x1000u    /* Fault Level, fault 0 (1 = active high)     */
#define FSTS_FFLAG0  0x0001u    /* Fault Flag, fault 0 (latched, W1C)         */
#define FSTS_FFPIN0  0x0100u    /* Filtered Fault Pin, fault 0 (live, RO)     */

/* ---- NVIC: FLEXPWM0_FAULT = IRQ 113 (bank 3, IRQ 96..127) ----------------- */
#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)
#define FAULT_IRQ  113
#define FAULT_BIT  (1u << (FAULT_IRQ - 96))

static volatile uint32_t fault_irq_count;

/* The fault IRQ is LEVEL (FFLAG & FIE).  Mask it AT THE SOURCE -- clear this fault's FIE, which
 * deasserts the line -- so one assertion counts exactly once with no latched NVIC pending (the
 * flag stays latched in FFLAG regardless).  Each test phase re-arms FIE by writing FCTRL. */
void flexpwm0_fault_handler(void)
{
    fault_irq_count++;
    PWM_FCTRL &= ~FCTRL_FIE0;
}

/* Bounded spin on a volatile condition; returns nonzero if the condition became true. */
#define SPIN_UNTIL(cond) do { int _g = 300000000; while (!(cond) && _g--) {} } while (0)

void cpu0_main(void)
{
    int okA, okB, okC;
    uint32_t base;

    LP_CTRL = (1u << 19);
    puts_("FAULT test\r\n");

    NVIC_ISER3 = FAULT_BIT;                      /* enable FLEXPWM0_FAULT once; the handler masks
                                                  * at the source (FIE), so it stays enabled */
    __asm__ volatile ("cpsie i");

    /* ---- A: detection + FIE-gated interrupt + clear interlock -------------- */
    PWM_FCTRL = FCTRL_FLVL0 | FCTRL_FIE0;      /* active-high, interrupt enabled, manual clear */
    puts_("FAULT ARMED-A\r\n");                 /* harness drives fault-input = 1 (assert) */
    SPIN_UNTIL(fault_irq_count >= 1);
    okA = (fault_irq_count == 1) &&
          (PWM_FSTS & FSTS_FFLAG0) &&           /* fault latched */
          (PWM_FSTS & FSTS_FFPIN0);             /* live pin mirrored */

    PWM_FSTS = FSTS_FFLAG0;                      /* W1C while STILL asserted -> must be refused */
    okA &= !!(PWM_FSTS & FSTS_FFLAG0);           /* the safety interlock: still set */

    puts_("FAULT ARMED-A-CLR\r\n");              /* harness drives fault-input = 0 (deassert) */
    SPIN_UNTIL(!(PWM_FSTS & FSTS_FFPIN0));       /* wait for the deassert to be visible */
    PWM_FSTS = FSTS_FFLAG0;                       /* now the W1C is allowed */
    okA &= !(PWM_FSTS & FSTS_FFLAG0);            /* cleared once the fault is gone */

    /* ---- B: FIE gates the interrupt (NVIC 113 still enabled) --------------- */
    PWM_FCTRL = FCTRL_FLVL0;                      /* active-high, FIE0 = 0 -> the only gate is FIE */
    base = fault_irq_count;
    puts_("FAULT ARMED-B\r\n");                  /* harness drives fault-input = 1 */
    SPIN_UNTIL(PWM_FSTS & FSTS_FFLAG0);          /* FFLAG SET = delivery proof */
    okB = (PWM_FSTS & FSTS_FFLAG0) &&            /* detected regardless of FIE */
          (fault_irq_count == base);            /* but NO interrupt (FIE gated it) */
    puts_("FAULT ARMED-B-CLR\r\n");              /* harness drives fault-input = 0 */
    SPIN_UNTIL(!(PWM_FSTS & FSTS_FFPIN0));
    PWM_FSTS = FSTS_FFLAG0;                       /* clear (inactive now) */

    /* ---- C: FLVL polarity, input held LOW, toggling ONLY FLVL ------------- *
     * The input idles LOW (harness drove 0 at B-CLR).  Toggling FLVL alone flips whether that
     * SAME level is a fault -- a synchronous discriminator that needs no operator race and no
     * timed absence-check: a write to FCTRL that faults takes its IRQ before the next
     * instruction, so the count read right after is deterministic.  (A HIGH-active idle-LOW is
     * NOT a fault; flip to LOW-active and the idle LOW becomes the fault.  A model that ignores
     * FLVL produces no fault on the second write -> FFLAG stays clear -> FAIL.) */
    base = fault_irq_count;
    PWM_FCTRL = FCTRL_FLVL0 | FCTRL_FIE0;         /* active-HIGH: the idle LOW is NOT a fault */
    okC = (fault_irq_count == base) &&            /* no IRQ taken (synchronous) */
          !(PWM_FSTS & FSTS_FFLAG0);              /* and nothing latched */
    PWM_FCTRL = FCTRL_FIE0;                        /* active-LOW: the SAME low input IS a fault */
    okC &= !!(PWM_FSTS & FSTS_FFLAG0) &&          /* fault latches */
           (fault_irq_count == base + 1);         /* + exactly one interrupt */

    puts_("  okA="); putdec(okA); puts_(" okB="); putdec(okB);
    puts_(" okC="); putdec(okC); puts_(" irq="); putdec(fault_irq_count); puts_("\r\n");
    puts_((okA && okB && okC) ? "FAULT PASS\r\n" : "FAULT FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[144] = {
    [0]  = (vec_t)0x20010000u,          /* initial MSP */
    [1]  = cpu0_main,                   /* Reset_Handler */
    [16 + FAULT_IRQ] = flexpwm0_fault_handler,  /* exception 129 = IRQ 113 */
};
