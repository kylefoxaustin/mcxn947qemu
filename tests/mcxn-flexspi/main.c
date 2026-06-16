/*
 * MCXN947 FlexSPI IP-command-done + NVIC interrupt test.
 *
 * Software-resets the controller (MCR0.SWRESET self-clears), enables the
 * IP-command-done interrupt (INTEN.IPCMDDONE) and the FlexSPI NVIC line
 * (IRQ 58), then launches an IP command (IPCMD.TRG).  The model completes the
 * command, sets INTR.IPCMDDONE and asserts the IRQ; the ISR acks it.  Prints
 * "FLEXSPI PASS" once the IP-command-done interrupt is delivered through the
 * IPCMD -> INTR -> NVIC -> handler path.
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

#define FLEXSPI 0x400C8000u
#define MCR0   (*(volatile uint32_t *)(FLEXSPI + 0x00))
#define INTEN  (*(volatile uint32_t *)(FLEXSPI + 0x10))
#define INTR   (*(volatile uint32_t *)(FLEXSPI + 0x14))
#define IPCMD  (*(volatile uint32_t *)(FLEXSPI + 0xB0))

#define MCR0_SWRESET   (1u << 0)
#define INTR_IPCMDDONE (1u << 0)
#define IPCMD_TRG      (1u << 0)

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define FLEXSPI_IRQ 58

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

static volatile uint32_t done_count;

void flexspi_handler(void)
{
    if (INTR & INTR_IPCMDDONE) {
        done_count++;
        INTR = INTR_IPCMDDONE;   /* write-1-to-clear */
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("FLEXSPI test\r\n");

    MCR0 = MCR0_SWRESET;             /* software reset, self-clears */
    while (MCR0 & MCR0_SWRESET) {
    }

    INTEN = INTR_IPCMDDONE;
    NVIC_ISER1 = (1u << (FLEXSPI_IRQ - 32));
    __asm__ volatile ("cpsie i");

    IPCMD = IPCMD_TRG;               /* launch an IP command */

    while (done_count < 1) {
    }

    puts_(done_count >= 1 ? "FLEXSPI PASS\r\n" : "FLEXSPI FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + FLEXSPI_IRQ] = flexspi_handler,  /* exception 74 = IRQ 58 */
};
