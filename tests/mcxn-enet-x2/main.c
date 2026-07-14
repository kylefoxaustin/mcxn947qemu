/*
 * MCXN947 two-instance cross-board ENET test (real wire, not loopback).
 *
 * The SAME firmware runs on two MCX QEMU instances joined by a shared
 * '-nic socket,mcast=...,model=mcxn-enet' segment.  Each node sets up the ENET
 * MAC + descriptor rings (NO MAC loopback), enables the Rx interrupt, and
 * repeatedly broadcasts an L2 frame while listening.  When a frame transmitted
 * by the PEER arrives over the socket, the Rx DMA writes it into a descriptor,
 * raises IRQ 139, and the ISR prints "ENETX2 PASS".  This exercises the real
 * qemu_send_packet -> socket -> peer receive-callback -> Rx descriptor path -
 * exactly what a board-to-board lab (MCX <-> i.MX93) uses.
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

#define ENET 0x40100000u
#define MAC_CONFIG  (*(volatile uint32_t *)(ENET + 0x000))
#define DMA_MODE    (*(volatile uint32_t *)(ENET + 0x1000))
#define TX_CTRL     (*(volatile uint32_t *)(ENET + 0x1104))
#define RX_CTRL     (*(volatile uint32_t *)(ENET + 0x1108))
#define TXDESC_LIST (*(volatile uint32_t *)(ENET + 0x1114))
#define RXDESC_LIST (*(volatile uint32_t *)(ENET + 0x111C))
#define TXDESC_TAIL (*(volatile uint32_t *)(ENET + 0x1120))
#define RXDESC_TAIL (*(volatile uint32_t *)(ENET + 0x1128))
#define TXRING_LEN  (*(volatile uint32_t *)(ENET + 0x112C))
#define RXRING_LEN  (*(volatile uint32_t *)(ENET + 0x1130))
#define DMA_INT_EN  (*(volatile uint32_t *)(ENET + 0x1134))
#define DMA_STATUS  (*(volatile uint32_t *)(ENET + 0x1160))

#define MAC_RE 0x1u
#define MAC_TE 0x2u
#define DMA_SWR 0x1u
#define TX_ST 0x1u
#define RX_SR 0x1u
#define STAT_RI 0x40u
#define INT_RIE 0x40u
#define INT_NIE 0x8000u

#define TDES2_IOC 0x80000000u
#define TDES3_OWN 0x80000000u
#define TDES3_FD  0x20000000u
#define TDES3_LD  0x10000000u
#define RDES3_OWN 0x80000000u
#define RDES3_IOC 0x40000000u
#define RDES3_BUF1V 0x01000000u

#define TXDESC 0x20008000u
#define TXBUF  0x20008100u
#define RXDESC 0x20008200u
#define RXBUF  0x20008300u
#define FRAME_LEN 64

/* Each node is IDENTIFIABLE on the wire, so a node cannot satisfy itself with its own
 * echo, and a receiver can say WHOSE frame it verified. */
#ifndef NODE_ID
#define NODE_ID 1
#endif
#define X2_ETHERTYPE 0x88B4u          /* not a real protocol; distinct from the lab3 set */
static uint8_t payload(int i) { return (uint8_t)((i * 7 + 0x31) & 0xFF); }

#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MEM8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

#define NVIC_ISER4 (*(volatile uint32_t *)0xE000E110u)
#define ENET_IRQ 139

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

static volatile uint32_t got_peer;
static volatile uint32_t got_bad;

void enet_handler(void)
{
    uint32_t st = DMA_STATUS;
    if (st & STAT_RI) {
        /*
         * ⭐ THIS USED TO BE, IN ITS ENTIRETY:   got_peer = 1;
         *
         *   Set on ANY receive interrupt.  It never looked at the frame -- not the
         *   ethertype, not who sent it, not one payload byte.  That is not "I received
         *   the peer's frame".  IT IS "SOMETHING ARRIVED".  And the file's own header
         *   comment claimed "when a frame transmitted by the PEER arrives ... the ISR
         *   prints ENETX2 PASS" -- A COMMENT CLAIMING A CHECK THE CODE DOES NOT MAKE.
         *
         *   holobench found this exact shape in my lab3 beacon ("I saw 0x88B5" is a
         *   statement about a FIELD, not a FRAME).  I fixed it there and DID NOT RUN
         *   THE CENSUS -- 91emulator's rule, broken the same night I quoted it:
         *
         *     ⭐ WHEN A GATE FINDS A BUG, DO NOT FIX IT.  FIND ITS SIBLINGS.
         *       A BUG CLASS FOUND ONCE IS A CENSUS YOU HAVE NOT RUN.
         *
         *   uart-link, spi-link and can-link all compare payload bytes.  THIS ONE DID
         *   NOT -- on the ENET path, which is the one fabric where a burst can outrun a
         *   ring, and where rt1180 found frames being DMA'd to guest physical address
         *   ZERO.  A frame that never landed would have passed this test.
         */
        uint32_t et = ((uint32_t)MEM8(RXBUF + 12) << 8) | MEM8(RXBUF + 13);
        uint32_t from = MEM8(RXBUF + 14);
        int ok = 1;
        int i;

        if (et != X2_ETHERTYPE || from == NODE_ID || from == 0) {
            ok = 0;                       /* not our protocol, or our own echo */
        } else {
            for (i = 15; i < FRAME_LEN; i++) {
                if (MEM8(RXBUF + i) != payload(i)) {
                    ok = 0;               /* arrived, but NOT INTACT */
                    break;
                }
            }
        }
        if (ok) {
            got_peer = 1;
        } else if (et == X2_ETHERTYPE && from != NODE_ID && from != 0) {
            got_bad = 1;                  /* our protocol, corrupt body: say so LOUDLY */
        }
        /* Re-arm the Rx descriptor so the node keeps a buffer available. */
        MEM32(RXDESC + 12) = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;
        RXDESC_TAIL = RXDESC + 16;
    }
    DMA_STATUS = st & STAT_RI;   /* write-1-to-clear */
}

static void arm_tx(void)
{
    MEM32(TXDESC + 8) = TDES2_IOC | FRAME_LEN;
    MEM32(TXDESC + 12) = TDES3_OWN | TDES3_FD | TDES3_LD | FRAME_LEN;
    TXDESC_TAIL = TXDESC + 16;   /* doorbell: broadcast the frame */
}

void cpu0_main(void)
{
    int i;
    volatile int d;

    LP_CTRL = CTRL_TE;
    puts_("ENETX2 test\r\n");

    DMA_MODE = DMA_SWR;
    while (DMA_MODE & DMA_SWR) {
    }

    /* Broadcast frame, IDENTIFIED and CHECKABLE: the body is the evidence. */
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + i) = 0xFF;                       /* dest = broadcast */
    }
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + 6 + i) = (uint8_t)(0x02 + i);    /* src MAC (locally administered) */
    }
    MEM8(TXBUF + 5) = 0xFF;
    MEM8(TXBUF + 11) = NODE_ID;                       /* ...unique per node */
    MEM8(TXBUF + 12) = (X2_ETHERTYPE >> 8) & 0xFF;
    MEM8(TXBUF + 13) = X2_ETHERTYPE & 0xFF;
    MEM8(TXBUF + 14) = NODE_ID;                       /* WHO sent this */
    for (i = 15; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = payload(i);                 /* WHAT they sent -- checked on RX */
    }
    MEM32(TXDESC + 0) = TXBUF;
    MEM32(TXDESC + 4) = 0;

    MEM32(RXDESC + 0) = RXBUF;
    MEM32(RXDESC + 4) = 0;
    MEM32(RXDESC + 8) = 0;
    MEM32(RXDESC + 12) = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;

    TXDESC_LIST = TXDESC; TXRING_LEN = 0;
    RXDESC_LIST = RXDESC; RXRING_LEN = 0;
    RX_CTRL = RX_SR;
    TX_CTRL = TX_ST;
    MAC_CONFIG = MAC_TE | MAC_RE;   /* NO loopback: frames go out the wire */
    DMA_INT_EN = INT_RIE | INT_NIE;

    NVIC_ISER4 = (1u << (ENET_IRQ - 128));
    __asm__ volatile ("cpsie i");

    RXDESC_TAIL = RXDESC + 16;

    /* Keep broadcasting for the whole run so BOTH nodes keep emitting frames
     * (a node must not go quiet once it has received, or it starves its peer).
     * Latch and print PASS the first time a peer frame arrives. */
    int printed = 0;
    for (i = 0; i < 100000; i++) {
        arm_tx();
        for (d = 0; d < 50000; d++) {
        }
        if (got_bad && !printed) {
            puts_("ENETX2 CORRUPT: peer frame arrived but the body is wrong\r\n");
            got_bad = 0;
        }
        if (got_peer && !printed) {
            puts_("ENETX2 PASS\r\n");
            printed = 1;
        }
    }

    if (!printed) {
        puts_("ENETX2 FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[160] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + ENET_IRQ] = enet_handler,  /* exception 155 = IRQ 139 */
};
