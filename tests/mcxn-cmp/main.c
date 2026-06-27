/*
 * MCXN947 CMP (LPCMP analog comparator) + NVIC interrupt test.
 *
 * The comparator output is OPERATOR-DRIVEN: QEMU has no analog stimulus, so the
 * level the +/- inputs would resolve to is injected at runtime via the
 * "comparator-output" QOM property (the test harness sets it over QMP, the way
 * a board-farm control plane would).  This guest enables CMP0, arms the
 * rising-edge interrupt (IER[CFR_IE]), enables the CMP0 NVIC line (IRQ 109),
 * prints "CMP ARMED" and spins.  When the operator drives the output high the
 * model latches CSR[CFR] and asserts IRQ 109; the ISR confirms the rising-edge
 * flag and that CSR[COUT] now reads high, then prints "CMP PASS".
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

#define CMP0 0x40051000u
#define CMP_CCR0 (*(volatile uint32_t *)(CMP0 + 0x08))
#define CMP_IER  (*(volatile uint32_t *)(CMP0 + 0x1C))
#define CMP_CSR  (*(volatile uint32_t *)(CMP0 + 0x20))

#define CCR0_CMP_EN (1u << 0)
#define IER_CFR_IE  (1u << 0)
#define CSR_CFR     (1u << 0)
#define CSR_COUT    (1u << 8)

#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)  /* IRQ 96..127 enable */
#define CMP0_IRQ 109

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
static volatile uint32_t saw_cout;

void cmp0_handler(void)
{
    uint32_t csr = CMP_CSR;
    if (csr & CSR_CFR) {
        if (csr & CSR_COUT) {
            saw_cout = 1;
        }
        irq_count++;
        CMP_CSR = CSR_CFR;   /* write-1-to-clear rising-edge flag, deassert IRQ */
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("CMP test\r\n");

    CMP_CCR0 = CCR0_CMP_EN;   /* enable comparator */
    CMP_IER  = IER_CFR_IE;    /* rising-edge interrupt enable */

    NVIC_ISER3 = (1u << (CMP0_IRQ - 96));
    __asm__ volatile ("cpsie i");

    puts_("CMP ARMED\r\n");   /* harness now injects comparator-output=true */

    while (irq_count < 1) {
    }

    if (irq_count >= 1 && saw_cout) {
        puts_("CMP PASS\r\n");
    } else {
        puts_("CMP FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[128] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + CMP0_IRQ] = cmp0_handler,  /* exception 125 = IRQ 109 */
};
