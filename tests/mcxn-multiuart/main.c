/* MCXN947 multi-FlexComm UART test: writes distinct banners to LPUART4
 * (FlexComm4 -> serial0) and LPUART2 (FlexComm2 -> serial1).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
static void tx(uint32_t base, const char *s)
{
    volatile uint32_t *ctrl = (volatile uint32_t *)(base + 0x18);
    volatile uint32_t *stat = (volatile uint32_t *)(base + 0x14);
    volatile uint32_t *data = (volatile uint32_t *)(base + 0x1C);
    *ctrl = (1u << 19);                 /* TE */
    while (*s) { while (!(*stat & (1u << 23))) {} *data = (uint8_t)*s++; }
}
void cpu0_main(void)
{
    tx(0x400B4000u, "FC4 hello\r\n");   /* LPUART4 -> serial0 */
    tx(0x40094000u, "FC2 hello\r\n");   /* LPUART2 -> serial1 */
    for (;;) {}
}
typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
