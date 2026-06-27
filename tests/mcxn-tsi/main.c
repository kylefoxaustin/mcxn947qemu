/*
 * MCXN947 TSI (Touch Sensing Input) + NVIC interrupt test.
 *
 * Enables TSI0, selects a scan channel via CONFIG[TSICH], enables the TSI
 * end-of-scan NVIC line (IRQ 101) and fires a software trigger (GENCS[SWTS]).
 * The model completes the scan, latches DATA[TSICNT] from the operator-driven
 * per-channel counter (default sample 0x0100) and asserts IRQ 101; the ISR
 * reads the count, checks DATA[EOSF], captures the value and clears the flag.
 * Prints "TSI PASS" once a count has been delivered through the full
 * scan -> DATA[EOSF] -> NVIC -> handler path with the selected-channel value.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

#define TSI0 0x40050000u
#define TSI_CONFIG (*(volatile uint32_t *)(TSI0 + 0x000))
#define TSI_GENCS  (*(volatile uint32_t *)(TSI0 + 0x008))
#define TSI_DATA   (*(volatile uint32_t *)(TSI0 + 0x100))

#define GENCS_TSIEN (1u << 5)
#define GENCS_SWTS  (1u << 7)
#define CONFIG_TSICH_SHIFT 1
#define DATA_TSICNT_MASK 0xFFFFu
#define DATA_EOSF   (1u << 27)

#define SCAN_CHANNEL 3u
#define TSI_DEFAULT_COUNT 0x0100u

#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)  /* IRQ 96..127 enable */
#define TSI_IRQ 101

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

static volatile uint32_t irq_count;
static volatile uint32_t last_count;

void tsi_handler(void)
{
    uint32_t d = TSI_DATA;
    if (d & DATA_EOSF) {
        last_count = d & DATA_TSICNT_MASK;
        irq_count++;
        TSI_DATA = DATA_EOSF;   /* write-1-to-clear end-of-scan, deassert IRQ */
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("TSI test\r\n");

    TSI_GENCS  = GENCS_TSIEN;                       /* enable module */
    TSI_CONFIG = SCAN_CHANNEL << CONFIG_TSICH_SHIFT; /* select channel 3 */

    NVIC_ISER3 = (1u << (TSI_IRQ - 96));
    __asm__ volatile ("cpsie i");

    TSI_GENCS = GENCS_TSIEN | GENCS_SWTS;          /* software trigger -> scan */
    while (irq_count < 1) {
    }

    /* A second scan to confirm the path re-arms cleanly. */
    TSI_GENCS = GENCS_TSIEN | GENCS_SWTS;
    while (irq_count < 2) {
    }

    if (irq_count >= 2 && last_count == TSI_DEFAULT_COUNT) {
        puts_("TSI PASS\r\n");
    } else {
        puts_("TSI FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[128] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + TSI_IRQ] = tsi_handler,  /* exception 117 = IRQ 101 */
};
