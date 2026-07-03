/*
 * MCXN947 CAN board-to-board link test (inter-QEMU FlexCAN over a chardev).
 *
 * Proves the MCX is a CAN b2b node on the fleet's generic can-host-chardev
 * backend (95's net/can/can_host_chardev.c): FlexCAN0 sits on an emulated
 * CAN bus, and a `-object can-host-chardev,canbus=canbus0,chardev=<sock>`
 * bridges that bus to a socket, so frames cross to a peer instance.  No
 * loopback (CTRL1.LPB clear) — TX message buffers put frames on the real bus.
 *
 * cpu0 transmits a data frame (std ID 0x321, 8 bytes) and waits for the peer's
 * reply frame (std ID 0x322) in an RX message buffer; the RX ISR verifies the
 * payload byte-for-byte.  Resends until the reply arrives (the peer may connect
 * after boot).  Prints "CAN LINK PASS" once the reply round-trips.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP4_BASE 0x400B4000u
#define LP4_STAT (*(volatile uint32_t *)(LP4_BASE + 0x14))
#define LP4_CTRL (*(volatile uint32_t *)(LP4_BASE + 0x18))
#define LP4_DATA (*(volatile uint32_t *)(LP4_BASE + 0x1C))
#define CTRL_TE  (1u << 19)
#define STAT_TDRE (1u << 23)

#define CAN0 0x400D4000u
#define CAN_MCR      (*(volatile uint32_t *)(CAN0 + 0x000))
#define CAN_CTRL1    (*(volatile uint32_t *)(CAN0 + 0x004))
#define CAN_RXMGMASK (*(volatile uint32_t *)(CAN0 + 0x010))
#define CAN_IMASK1   (*(volatile uint32_t *)(CAN0 + 0x028))
#define CAN_IFLAG1   (*(volatile uint32_t *)(CAN0 + 0x030))
#define MB_CS(n)  (*(volatile uint32_t *)(CAN0 + 0x80 + (n) * 0x10 + 0x0))
#define MB_ID(n)  (*(volatile uint32_t *)(CAN0 + 0x80 + (n) * 0x10 + 0x4))
#define MB_W0(n)  (*(volatile uint32_t *)(CAN0 + 0x80 + (n) * 0x10 + 0x8))
#define MB_W1(n)  (*(volatile uint32_t *)(CAN0 + 0x80 + (n) * 0x10 + 0xC))

#define CODE_RX_EMPTY (0x4u << 24)
#define CODE_TX_DATA  (0xCu << 24)
#define CODE_RX_FULL  0x2u
#define DLC8          (8u << 16)

#define RX_MB 4
#define TX_MB 0
#define TX_ID_STD  0x321u              /* MCX -> peer std ID */
#define RX_ID_STD  0x322u              /* peer -> MCX std ID */
#define TXD0 0xDEADBEEFu
#define TXD1 0xCAFEBABEu
#define RXD0 0x11223344u               /* expected reply payload */
#define RXD1 0x55667788u

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)
#define CAN0_IRQ 62

static void c_putc(char c) { while (!(LP4_STAT & STAT_TDRE)) {} LP4_DATA = (uint8_t)c; }
static void c_puts(const char *s) { while (*s) { c_putc(*s++); } }
static void delay(uint32_t n) { for (volatile uint32_t i = 0; i < n; i++) { } }

static volatile int got_reply;
static volatile int bad_reply;

void can0_isr(void)
{
    if (CAN_IFLAG1 & (1u << RX_MB)) {
        uint32_t code = (MB_CS(RX_MB) >> 24) & 0xF;
        uint32_t id   = MB_ID(RX_MB) >> 18;         /* std ID field [28:18] */
        uint32_t d0   = MB_W0(RX_MB);
        uint32_t d1   = MB_W1(RX_MB);
        CAN_IFLAG1 = (1u << RX_MB);                 /* W1C */
        MB_CS(RX_MB) = CODE_RX_EMPTY;               /* re-arm RX */
        if (code == CODE_RX_FULL && id == RX_ID_STD) {
            if (d0 == RXD0 && d1 == RXD1) {
                got_reply = 1;
            } else {
                bad_reply = 1;
            }
        }
    }
}

void cpu0_main(void)
{
    LP4_CTRL = CTRL_TE;
    c_puts("CAN LINK test\r\n");

    CAN_MCR = 0;                 /* enable module, leave freeze, ready */
    CAN_CTRL1 = 0;               /* NO loopback: frames go on the real bus */
    CAN_RXMGMASK = 0;            /* accept any ID */

    MB_CS(RX_MB) = CODE_RX_EMPTY;               /* arm RX */
    CAN_IMASK1 = (1u << RX_MB);
    NVIC_ISER1 = (1u << (CAN0_IRQ - 32));
    __asm__ volatile ("cpsie i");

    /* Transmit our frame + wait for the peer's reply; resend across the window
     * (the peer may connect after we boot; a frame sent to an empty bus drops). */
    for (uint32_t tries = 0; !got_reply && tries < 4000; tries++) {
        MB_W0(TX_MB) = TXD0;
        MB_W1(TX_MB) = TXD1;
        MB_ID(TX_MB) = TX_ID_STD << 18;
        MB_CS(TX_MB) = CODE_TX_DATA | DLC8;     /* writing CS transmits */
        delay(300000);
    }

    /* The reply proves the peer is now attached; send a burst so the peer also
     * receives our frames (exercises MCX->peer, not just peer->MCX). */
    for (int i = 0; i < 40 && got_reply; i++) {
        MB_W0(TX_MB) = TXD0;
        MB_W1(TX_MB) = TXD1;
        MB_ID(TX_MB) = TX_ID_STD << 18;
        MB_CS(TX_MB) = CODE_TX_DATA | DLC8;
        delay(300000);
    }

    if (got_reply) {
        c_puts("CAN LINK PASS\r\n");
    } else {
        c_puts(bad_reply ? "CAN LINK FAIL (payload)\r\n"
                         : "CAN LINK FAIL (no reply)\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,
    [1]  = cpu0_main,
    [16 + CAN0_IRQ] = can0_isr,
};
