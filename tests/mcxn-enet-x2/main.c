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

void enet_handler(void)
{
    uint32_t st = DMA_STATUS;
    if (st & STAT_RI) {
        got_peer = 1;
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

    /* Broadcast frame with a recognizable payload. */
    for (i = 0; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = (uint8_t)(0x50 + i);
    }
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + i) = 0xFF;   /* broadcast dest */
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
