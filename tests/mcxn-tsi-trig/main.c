/*
 * TSI SCAN ON A TIMER -- the touch-sensing hardware-trigger path.
 *
 *     LPTMR0 compare  ->  INPUTMUX (TSI_TRIG selector 0 = LPTMR0)
 *                     ->  TSI0 scan (GENCS[STM] = hardware-trigger mode)
 *                     ->  DATA[EOSF] + DATA[TSICNT].
 *
 * A low-power touch loop wakes the TSI periodically off a timer with no CPU involvement --
 * the touch-sensing counterpart of the ADC's "convert on a timer tick".  It was inert:
 * TSI_TRIG was a stored INPUTMUX selector reaching nothing, and GENCS[STM] (hardware-trigger
 * mode) merely faked one scan at the moment it was written, with no trigger source behind it.
 *
 *   PHASE 1 (GENCS[STM]=0, software mode): the LPTMR-routed trigger MUST be ignored
 *           -- DATA[EOSF] stays clear (a scan comes only from GENCS[SWTS] in this mode).
 *   PHASE 2 (GENCS[STM]=1, hardware mode): each LPTMR compare drives a scan -- DATA[EOSF]
 *           sets and DATA[TSICNT] reads the operator-set electrode count.
 *
 * The CPU never writes GENCS[SWTS].  Selector 0 = LPTMR0 (kINPUTMUX_Lptmr0ToTsiTrigger = 0).
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

/* ---- TSI0 (base 0x40050000) ---------------------------------------------- */
#define TSI0 0x40050000u
#define TSI_CONFIG (*(volatile uint32_t *)(TSI0 + 0x000))
#define TSI_GENCS  (*(volatile uint32_t *)(TSI0 + 0x008))
#define TSI_DATA   (*(volatile uint32_t *)(TSI0 + 0x100))
#define GENCS_STM   (1u << 3)
#define GENCS_TSIEN (1u << 5)
#define DATA_EOSF   (1u << 27)
#define DATA_TSICNT_MASK 0xFFFFu
#define TSI_COUNT_DEFAULT 0x0100u    /* the model's documented electrode count */

/* ---- LPTMR0 (base 0x4004A000) -------------------------------------------- */
#define LPTMR0 0x4004A000u
#define LPTMR_CSR (*(volatile uint32_t *)(LPTMR0 + 0x0))
#define LPTMR_PSR (*(volatile uint32_t *)(LPTMR0 + 0x4))
#define LPTMR_CMR (*(volatile uint32_t *)(LPTMR0 + 0x8))
#define LPTMR_CSR_TEN  (1u << 0)
#define LPTMR_PSR_PBYP (1u << 2)

/* INPUTMUX0: TSI_TRIG @0x4A0. */
#define INPUTMUX0 0x40006000u
#define TSI_TRIG  (*(volatile uint32_t *)(INPUTMUX0 + 0x4A0))
#define TRIG_SEL_LPTMR0 0u

void cpu0_main(void)
{
    volatile int d;
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("TSI-TRIG test\r\n");

    /* Enable TSI0 in SOFTWARE-trigger mode (STM=0), channel 0. */
    TSI_GENCS  = GENCS_TSIEN;
    TSI_CONFIG = 0;
    TSI_TRIG   = TRIG_SEL_LPTMR0;

    /* LPTMR0 periodic. */
    LPTMR_PSR = LPTMR_PSR_PBYP;
    LPTMR_CMR = 200;
    LPTMR_CSR = LPTMR_CSR_TEN;

    /* PHASE 1 -- software mode: the routed trigger must NOT scan. */
    for (d = 0; d < 400000; d++) {
    }
    if (TSI_DATA & DATA_EOSF) {
        puts_("  FAIL: LPTMR trigger scanned in SOFTWARE mode (STM=0)\r\n");
        ok = 0;
    } else {
        puts_("  phase1: software mode -> trigger ignored (correct)\r\n");
    }

    /* PHASE 2 -- hardware mode: each LPTMR compare drives a scan. */
    TSI_GENCS = GENCS_TSIEN | GENCS_STM;
    for (d = 0; d < 20000000 && !(TSI_DATA & DATA_EOSF); d++) {
    }
    LPTMR_CSR = 0;

    puts_("  phase2 DATA="); puthex(TSI_DATA); puts_("\r\n");
    ok &= !!(TSI_DATA & DATA_EOSF);                        /* a scan completed */
    ok &= ((TSI_DATA & DATA_TSICNT_MASK) == TSI_COUNT_DEFAULT);  /* operator count */
    puts_(ok ? "  phase2: LPTMR paced a scan (correct)\r\n"
             : "  phase2: FAILED\r\n");

    puts_(ok ? "TSI-TRIG PASS\r\n" : "TSI-TRIG FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
