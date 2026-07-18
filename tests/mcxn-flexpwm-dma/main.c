/*
 * MCXN947 eFlexPWM value-register DMA (reload-driven, stock-driver-shaped).
 *
 * A DMA-driven FlexPWM control loop (PWM_SetupPwmDMA / the fsl_pwm value-DMA path)
 * arms an eDMA channel at the VALx registers, sets SM0.DMAEN[VALDE], and then NEVER
 * writes the duty-cycle words itself: on every RELOAD the FlexPWM raises the value
 * DMA request (mux source FlexPWM0 Val0 = 43), the eDMA writes ONE VALx word, and the
 * new duty cycle takes effect at the following reload.  Before that request line was
 * wired, VALDE drove nothing: the FlexPWM raised no request, a channel that set ERQ
 * and waited for the reload waited forever, and a value-DMA loop hung.
 *
 * The oracle the FlexPWM model does not own, on two axes:
 *   1. DATA -- the CPU seeds VAL3 with a sentinel and never writes it again; after the
 *      transfer VAL3 must hold the LAST duty word, carried there purely by reload-driven
 *      DMA.  A model that raised no request leaves the sentinel (and never sets DONE).
 *   2. RATE -- the request is EDGE-driven by the reload, so it must move exactly ONE word
 *      per reload.  Under -icount the whole 8-word transfer therefore takes ~8 carrier
 *      periods of virtual time, measured against SysTick (an Arm core timer, independent
 *      of the PWM model).  A model that drained the whole buffer in a single reload would
 *      finish in ~1 period -- caught by the elapsed-time floor.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP_CTRL (*(volatile uint32_t *)0x400B4018u)
#define LP_STAT (*(volatile uint32_t *)0x400B4014u)
#define LP_DATA (*(volatile uint32_t *)0x400B401Cu)
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

/* ---- eFlexPWM0 (SM0) ------------------------------------------------------ */
#define PWM0            0x400CE000u
#define PWM_SM0_INIT    (*(volatile uint16_t *)(PWM0 + 0x02))
#define PWM_SM0_CTRL    (*(volatile uint16_t *)(PWM0 + 0x06))
#define PWM_SM0_VAL1    (*(volatile uint16_t *)(PWM0 + 0x0E))
#define PWM_SM0_VAL3    (*(volatile uint16_t *)(PWM0 + 0x16))
#define PWM_SM0_STS     (*(volatile uint16_t *)(PWM0 + 0x24))
#define PWM_SM0_DMAEN   (*(volatile uint16_t *)(PWM0 + 0x28))
#define PWM_MCTRL       (*(volatile uint16_t *)(PWM0 + 0x188))
#define PWM_VAL3_ADDR   (PWM0 + 0x16)

#define DMAEN_VALDE     (1u << 9)      /* Value Registers DMA Enable            */
#define MCTRL_LDOK_SM0  (1u << 0)      /* load the buffered VAL/INIT registers  */
#define MCTRL_RUN_SM0   (1u << 8)      /* submodule-0 counter run               */
#define STS_RF          (1u << 12)     /* reload flag                           */

/* ---- SysTick (independent core timer, counts DOWN) ------------------------ */
#define SYST_CSR (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018u)

/* ---- DMA0 ----------------------------------------------------------------- */
#define DMA0 0x40080000u
#define CH(n) (DMA0 + 0x1000u * ((n) + 1))
#define CH_CSR(n)    (*(volatile uint32_t *)(CH(n) + 0x00))
#define CH_INT(n)    (*(volatile uint32_t *)(CH(n) + 0x08))
#define CH_MUX(n)    (*(volatile uint32_t *)(CH(n) + 0x14))
#define TCD_SADDR(n) (*(volatile uint32_t *)(CH(n) + 0x20))
#define TCD_SOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x24))
#define TCD_ATTR(n)  (*(volatile uint16_t *)(CH(n) + 0x26))
#define TCD_NBYTES(n)(*(volatile uint32_t *)(CH(n) + 0x28))
#define TCD_SLAST(n) (*(volatile uint32_t *)(CH(n) + 0x2C))
#define TCD_DADDR(n) (*(volatile uint32_t *)(CH(n) + 0x30))
#define TCD_DOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x34))
#define TCD_CITER(n) (*(volatile uint16_t *)(CH(n) + 0x36))
#define TCD_DLAST(n) (*(volatile uint32_t *)(CH(n) + 0x38))
#define TCD_CSR(n)   (*(volatile uint16_t *)(CH(n) + 0x3C))
#define TCD_BITER(n) (*(volatile uint16_t *)(CH(n) + 0x3E))
#define ATTR_16BIT   0x0101u           /* SSIZE=DSIZE=1 -> 2-byte element       */
#define CSR_ERQ      (1u << 0)
#define CSR_DONE     (1u << 30)
#define TCD_INTMAJOR (1u << 1)
#define TCD_DREQ     (1u << 3)          /* auto-clear ERQ at major completion    */
#define DMAREQ_PWM0_VAL 43u
#define CHAN 0

#define NW      8
#define PERIOD  3000u                  /* VAL1+1 counter ticks (PRSC=0)          */
#define SENTINEL 0xBEEFu

/* Duty words, distinct and non-round, all < PERIOD.  const -> lives in flash;
 * the eDMA reads them from there (no CPU write to VAL3 after arming). */
static const uint16_t duty[NW] = {
    0x0111, 0x02A3, 0x0587, 0x08F1, 0x0B29, 0x0640, 0x09BB, 0x0A5C,
};

void cpu0_main(void)
{
    int ok = 1;
    uint32_t t0, t1, elapsed;
    int g;

    LP_CTRL = (1u << 19);
    puts_("FLEXPWM-DMA test\r\n");

    /* SM0: INIT=0, period = PERIOD (VAL1 = PERIOD-1), PRSC=0. */
    PWM_SM0_INIT = 0;
    PWM_SM0_CTRL = 0;
    PWM_SM0_VAL1 = PERIOD - 1;
    PWM_SM0_VAL3 = SENTINEL;            /* the last CPU write to VAL3            */

    /* Arm ch0: duty[] -> VAL3, one 16-bit word per reload request, NW words. */
    CH_CSR(CHAN)     = CSR_DONE;        /* W1C any stale DONE                    */
    CH_INT(CHAN)     = 1u;              /* W1C any stale major-loop interrupt    */
    TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)duty;
    TCD_SOFF(CHAN)   = 2;
    TCD_ATTR(CHAN)   = ATTR_16BIT;
    TCD_NBYTES(CHAN) = 2;
    TCD_SLAST(CHAN)  = 0;
    TCD_DADDR(CHAN)  = PWM_VAL3_ADDR;
    TCD_DOFF(CHAN)   = 0;               /* dst fixed on VAL3                     */
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = NW;
    TCD_BITER(CHAN)  = NW;
    TCD_CSR(CHAN)    = TCD_INTMAJOR | TCD_DREQ;
    CH_MUX(CHAN)     = DMAREQ_PWM0_VAL;
    CH_CSR(CHAN)     = CSR_ERQ;

    /* SysTick free-running on the processor clock (same clock the PWM counts). */
    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0;
    SYST_CSR = 0x5;                     /* ENABLE | CLKSOURCE=processor          */

    t0 = SYST_CVR;

    /* Enable value DMA, load the buffered regs, and start the counter. */
    PWM_SM0_DMAEN = DMAEN_VALDE;
    PWM_MCTRL = MCTRL_LDOK_SM0 | MCTRL_RUN_SM0;

    /* Wait for the whole transfer (bounded so a dead request line FAILs, not hangs). */
    g = 200000000;
    while (!(CH_CSR(CHAN) & CSR_DONE) && g--) {
    }
    t1 = SYST_CVR;

    PWM_MCTRL = 0;                      /* stop the counter                      */

    /* SysTick counts down; neither sample wrapped over ~NW*PERIOD ticks. */
    elapsed = (t0 - t1) & 0x00FFFFFFu;

    ok &= (CH_CSR(CHAN) & CSR_DONE) && !(CH_CSR(CHAN) & CSR_ERQ) && (CH_INT(CHAN) & 1u);
    ok &= (PWM_SM0_VAL3 == duty[NW - 1]);           /* data: DMA carried it      */
    ok &= (elapsed >= (uint32_t)(NW - 1) * PERIOD); /* rate: one word per reload */

    puts_(ok ? "FLEXPWM-DMA PASS\r\n" : "FLEXPWM-DMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
