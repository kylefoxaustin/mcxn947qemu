/*
 * MCXN947 UART board-to-board PASSTHROUGH (raw echo server, no protocol).
 *
 * A protocol-free alternative to the uartlink self-test for a holobench
 * "type on one console, see on the other" lab: the MCX brings up the link
 * UART (FlexComm2 / LPUART2 = serial_hd(1), wired to the peer over a chardev
 * socket) and simply ECHOES every received byte straight back out — and mirrors
 * it to the FlexComm4 console (serial_hd(0)) so the lab log shows the traffic.
 *
 * There is no handshake and no timeout, so the peer (e.g. a full-distro Linux
 * i.MX board that takes minutes to boot in a co-launched lab) can drive the
 * exchange whenever it is ready: send bytes -> get them echoed byte-exact.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP4_BASE 0x400B4000u                     /* console  = serial_hd(0) */
#define LP4_STAT (*(volatile uint32_t *)(LP4_BASE + 0x14))
#define LP4_CTRL (*(volatile uint32_t *)(LP4_BASE + 0x18))
#define LP4_DATA (*(volatile uint32_t *)(LP4_BASE + 0x1C))

#define LP2_BASE 0x40094000u                     /* link UART = serial_hd(1) */
#define LP2_STAT (*(volatile uint32_t *)(LP2_BASE + 0x14))
#define LP2_CTRL (*(volatile uint32_t *)(LP2_BASE + 0x18))
#define LP2_DATA (*(volatile uint32_t *)(LP2_BASE + 0x1C))

#define CTRL_RE   0x00040000u
#define CTRL_TE   0x00080000u
#define STAT_RDRF 0x00200000u
#define STAT_TDRE 0x00800000u

static void c_putc(char c) { while (!(LP4_STAT & STAT_TDRE)) {} LP4_DATA = (uint8_t)c; }
static void c_puts(const char *s) { while (*s) { c_putc(*s++); } }

void cpu0_main(void)
{
    LP4_CTRL = CTRL_TE;                           /* console TX */
    c_puts("UART PASSTHROUGH ready\r\n");

    LP2_CTRL = CTRL_RE | CTRL_TE;                 /* link RX + TX */

    for (;;) {
        if (LP2_STAT & STAT_RDRF) {
            uint8_t b = (uint8_t)LP2_DATA;         /* byte from the peer */
            while (!(LP2_STAT & STAT_TDRE)) { }
            LP2_DATA = b;                          /* echo it straight back */
            while (!(LP4_STAT & STAT_TDRE)) { }
            LP4_DATA = b;                          /* + mirror to the console */
        }
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[48] = {
    [0] = (vec_t)0x20010000u,
    [1] = cpu0_main,
};
