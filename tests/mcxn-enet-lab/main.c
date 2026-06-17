/*
 * MCXN947 cross-board ENET L2 lab emitter (for the holobench mcx93-eth lab, M1).
 *
 * Brings up the ENET-QoS MAC + descriptor rings (no MAC loopback), then on a
 * shared L2 segment it (a) periodically BROADCASTS an experimental-ethertype
 * (0x88B5) frame from a distinct MCX source MAC, and (b) RECEIVES peer frames
 * and prints each one's ethertype + source MAC to the console.  Paired with the
 * i.MX93 FEC node (which emits/sniffs the same 0x88B5 broadcast via AF_PACKET),
 * this proves M1 = raw L2 frames seen in BOTH directions over the wire.
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
#define ETHERTYPE 0x88B5u   /* IEEE local-experimental EtherType 1 */

#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MEM8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

#define NVIC_ISER4 (*(volatile uint32_t *)0xE000E110u)
#define ENET_IRQ 139

/* Distinct MCX source MAC (locally administered). */
static const uint8_t MCX_MAC[6] = { 0x02, 0x4D, 0x43, 0x58, 0x00, 0x01 };

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

static void puthex2(uint8_t v)
{
    const char *h = "0123456789abcdef";
    putc_(h[v >> 4]);
    putc_(h[v & 0xF]);
}

static volatile uint32_t got;

void enet_handler(void)
{
    uint32_t st = DMA_STATUS;
    if (st & STAT_RI) {
        got = 1;
    }
    DMA_STATUS = st & STAT_RI;
}

static void arm_tx(void)
{
    MEM32(TXDESC + 8) = TDES2_IOC | FRAME_LEN;
    MEM32(TXDESC + 12) = TDES3_OWN | TDES3_FD | TDES3_LD | FRAME_LEN;
    TXDESC_TAIL = TXDESC + 16;
}

static void rearm_rx(void)
{
    MEM32(RXDESC + 12) = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;
    RXDESC_TAIL = RXDESC + 16;
}

void cpu0_main(void)
{
    int i;
    volatile int d;

    LP_CTRL = CTRL_TE;
    puts_("ENET-LAB up: MCX broadcasting ethertype 0x88B5\r\n");

    DMA_MODE = DMA_SWR;
    while (DMA_MODE & DMA_SWR) {
    }

    /* Build a broadcast 0x88B5 frame from the MCX MAC, payload "MCX". */
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + i) = 0xFF;               /* dest = broadcast */
    }
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + 6 + i) = MCX_MAC[i];     /* src = MCX */
    }
    MEM8(TXBUF + 12) = (ETHERTYPE >> 8) & 0xFF;
    MEM8(TXBUF + 13) = ETHERTYPE & 0xFF;
    MEM8(TXBUF + 14) = 'M'; MEM8(TXBUF + 15) = 'C'; MEM8(TXBUF + 16) = 'X';
    for (i = 17; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = 0;
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
    MAC_CONFIG = MAC_TE | MAC_RE;   /* no loopback: frames go out the wire */
    DMA_INT_EN = INT_RIE | INT_NIE;

    NVIC_ISER4 = (1u << (ENET_IRQ - 128));
    __asm__ volatile ("cpsie i");

    rearm_rx();

    for (i = 0; ; i++) {
        arm_tx();
        if (got) {
            got = 0;
            /* Report the received peer frame: ethertype + source MAC. */
            uint32_t et = ((uint32_t)MEM8(RXBUF + 12) << 8) | MEM8(RXBUF + 13);
            puts_("ENET-LAB rx: ethertype 0x");
            puthex2((et >> 8) & 0xFF); puthex2(et & 0xFF);
            puts_(" src ");
            for (d = 0; d < 6; d++) {
                puthex2(MEM8(RXBUF + 6 + d));
                if (d < 5) {
                    putc_(':');
                }
            }
            puts_("\r\n");
            rearm_rx();
        }
        for (d = 0; d < 60000; d++) {
        }
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[160] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + ENET_IRQ] = enet_handler,  /* exception 155 = IRQ 139 */
};
