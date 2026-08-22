/*
 * MCXN947 migrate-mid-interrupt regression (pre-upstream Fable item #5).
 *
 * Verifies that a LEVEL interrupt asserted at save time is still live on the
 * destination after a migrate round-trip: the guest holds the DAC's FIFO-empty
 * IRQ high (the FIFO is empty at reset, so enabling the empty interrupt asserts
 * the line immediately — no timing, no data), re-arms it at the NVIC every loop
 * and counts fires in the ISR.  A "fire" happens only while the line is driven
 * HIGH at re-enable time, so the counter keeps climbing after migration only if
 * the level survived the round-trip.  It catches a broken/missing vmstate for
 * the IRQ-determining registers (GCR/IER) — drop them from vmstate and the
 * destination's line is low and the counter freezes.
 *
 * Note on the defence: the interrupt line survives here for TWO independent
 * reasons.  The ARMv7M NVIC vmstate saves each input line's level
 * (VMSTATE_UINT8(level, VecInfo)) and restores it, AND the peripheral carries a
 * post_load that re-drives its own line from the restored registers.  The NVIC
 * save alone covers NVIC-connected IRQs, so this test does not isolate the
 * post_load; the post_loads are kept as belt-and-suspenders (and are the ONLY
 * defence for non-NVIC device-to-device level outputs, e.g. eDMA request lines).
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
    const char *h = "0123456789abcdef";
    int i;
    for (i = 28; i >= 0; i -= 4) {
        putc_(h[(v >> i) & 0xF]);
    }
}

/* DAC0 @ 0x4010F000, IRQ 106. */
#define DAC0      0x4010F000u
#define DAC_GCR   (*(volatile uint32_t *)(DAC0 + 0x0C))
#define DAC_FSR   (*(volatile uint32_t *)(DAC0 + 0x18))
#define DAC_IER   (*(volatile uint32_t *)(DAC0 + 0x1C))
#define GCR_DACEN  (1u << 0)
#define GCR_FIFOEN (1u << 3)
#define FSR_EMPTY  (1u << 1)
#define IER_EMPTY  (1u << 1)   /* IER bit aligns with FSR (dac_update_irq: fsr & ier) */

/* NVIC: IRQ 106 -> ISER3/ICER3 bit (106-96)=10; exception 16+106=122. */
#define NVIC_ISER3 (*(volatile uint32_t *)0xE000E10Cu)
#define NVIC_ICER3 (*(volatile uint32_t *)0xE000E18Cu)
#define DAC0_BIT   (1u << (106 - 96))

static volatile uint32_t fires;

void dac0_handler(void)
{
    fires++;
    NVIC_ICER3 = DAC0_BIT;   /* disable at NVIC; leave DAC empty -> line stays HIGH */
}

void cpu0_main(void)
{
    uint32_t t;

    LP_CTRL = CTRL_TE;
    puts_("MIRQ up\r\n");

    /* Enable the DAC FIFO; empty at reset -> FSR_EMPTY.  Enable the empty
     * interrupt -> (FSR & IER) != 0 -> the IRQ line is driven HIGH. */
    DAC_GCR = GCR_DACEN | GCR_FIFOEN;
    DAC_IER = IER_EMPTY;

    __asm__ volatile ("cpsie i");

    for (t = 0; ; t++) {
        volatile int d;
        NVIC_ISER3 = DAC0_BIT;          /* (re-)arm: fires now iff line is HIGH */
        for (d = 0; d < 40000; d++) {
        }
        puts_("MIRQ t="); puthex(t);
        puts_(" fires="); puthex(fires); puts_("\r\n");
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[160] = {
    [0]   = (vec_t)0x20010000u,   /* initial MSP */
    [1]   = cpu0_main,            /* Reset_Handler */
    [16 + 106] = dac0_handler,    /* DAC0_IRQn = 106 */
};
