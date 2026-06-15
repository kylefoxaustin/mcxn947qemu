/*
 * MCXN947 MRT + LPTMR interrupt smoke test.
 *
 * Runs MRT channel 0 (repeat, down-counter) and LPTMR0 (up-count to compare),
 * each delivering periodic interrupts through the NVIC, and counts them in
 * their ISRs.  Prints "TIMERS PASS" once both have fired several times.
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

#define MRT0 0x40013000u
#define MRT_CH0_INTVAL (*(volatile uint32_t *)(MRT0 + 0x0))
#define MRT_CH0_CTRL   (*(volatile uint32_t *)(MRT0 + 0x8))
#define MRT_CH0_STAT   (*(volatile uint32_t *)(MRT0 + 0xC))

#define LPTMR0 0x4004A000u
#define LPT_CSR (*(volatile uint32_t *)(LPTMR0 + 0x0))
#define LPT_PSR (*(volatile uint32_t *)(LPTMR0 + 0x4))
#define LPT_CMR (*(volatile uint32_t *)(LPTMR0 + 0x8))

#define NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100u + 4 * (n)))
#define MRT0_IRQ   30
#define LPTMR0_IRQ 143

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

static volatile uint32_t mrt_count, lptmr_count;

void mrt0_handler(void)
{
    MRT_CH0_STAT = 0x1;       /* clear INTFLAG (W1C) */
    mrt_count++;
}
void lptmr0_handler(void)
{
    LPT_CSR |= 0x80;          /* clear TCF (W1C) */
    lptmr_count++;
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("TIMERS test\r\n");

    /* MRT ch0: repeat mode + interrupt, 2000-count interval. */
    MRT_CH0_CTRL = 0x1;       /* INTEN, MODE=repeat */
    NVIC_ISER(0) = (1u << MRT0_IRQ);
    MRT_CH0_INTVAL = 2000;    /* load + start */

    /* LPTMR0: bypass prescale, compare 2000, enable + interrupt. */
    LPT_PSR = 0x4;            /* PBYP */
    LPT_CMR = 2000;
    NVIC_ISER(LPTMR0_IRQ / 32) = (1u << (LPTMR0_IRQ % 32));
    LPT_CSR = 0x1 | 0x40;     /* TEN | TIE */

    __asm__ volatile ("cpsie i");

    while (mrt_count < 3 || lptmr_count < 3) {
    }

    puts_("TIMERS PASS\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[192] = {
    [0] = (vec_t)0x20010000u,
    [1] = cpu0_main,
    [16 + MRT0_IRQ]   = mrt0_handler,
    [16 + LPTMR0_IRQ] = lptmr0_handler,
};
