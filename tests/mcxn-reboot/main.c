/*
 * MCXN947 dual-Cortex-M33 release smoke test.
 *
 * cpu0 boots from flash, prints a banner over LPUART4, builds a vector table
 * for cpu1 in SRAM, then releases cpu1 via the SYSCON CPUCTRL/CPBOOT handover.
 * cpu1 boots from that vector table, writes a magic word and prints its own
 * banner.  Seeing BOTH banners proves cpu0 released cpu1.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE   0x400B4000u
#define LPUART_STAT    (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LPUART_CTRL    (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LPUART_DATA    (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE        (1u << 19)
#define STAT_TDRE      (1u << 23)

#define SYSCON_BASE    0x40000000u
#define SYSCON_CPUCTRL (*(volatile uint32_t *)(SYSCON_BASE + 0x800))
#define SYSCON_CPBOOT  (*(volatile uint32_t *)(SYSCON_BASE + 0x804))
#define CPU1CLKEN      (1u << 3)
#define PROT_KEY       0xC0C40000u           /* CPUCTRL write-enable key */

#define CPU1_VT        0x20002000u           /* cpu1 vector table (SRAM) */
#define CPU1_SP        0x20020000u
#define MAGIC_ADDR     (*(volatile uint32_t *)0x20001000u)
#define MAGIC          0xC0FFEE11u

static void uart_putc(char c)
{
    while (!(LPUART_STAT & STAT_TDRE)) {
    }
    LPUART_DATA = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

void cpu1_main(void)
{
    MAGIC_ADDR = MAGIC;
    uart_puts("CPU1 up\r\n");
    for (;;) {
    }
}

void cpu0_main(void)
{
    volatile uint32_t *vt = (volatile uint32_t *)CPU1_VT;

    LPUART_CTRL = CTRL_TE;
    uart_puts("CPU0 up\r\n");

    /* Stage cpu1's vector table: [0]=initial SP, [1]=reset PC (thumb). */
    vt[0] = CPU1_SP;
    vt[1] = ((uint32_t)&cpu1_main) | 1u;

    /* Release cpu1: point CPBOOT at its vector table, clock on + reset off. */
    SYSCON_CPBOOT  = CPU1_VT;
    SYSCON_CPUCTRL = PROT_KEY | CPU1CLKEN;

    for (;;) {
    }
}

/* cpu0 reset vector table at 0x0 (flash): initial MSP + reset handler.
 * Entries are function-pointer typed so the reset PC carries the thumb bit the
 * toolchain sets on function symbols (a static '| 1' on an address is not a
 * constant initializer). */
typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t cpu0_vectors[] = {
    (vec_t)0x20010000u,             /* initial MSP for cpu0 */
    cpu0_main,                      /* Reset_Handler (thumb bit already set) */
};
