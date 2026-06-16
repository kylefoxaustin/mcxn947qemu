/*
 * MCXN947 ENET MAC frame loopback + NVIC interrupt test.
 *
 * Exercises the real DesignWare ENET-QoS descriptor-ring DMA: builds a Tx
 * descriptor + buffer and an Rx descriptor + buffer in SRAM, puts the MAC in
 * internal loopback (MAC_CONFIGURATION.LM), starts the Tx/Rx DMA, and rings the
 * transmit doorbell (TXDESC_TAIL).  The model reads the Tx descriptor, sends the
 * frame, loops it back into the Rx descriptor's buffer, writes the descriptor
 * back to the CPU, and raises the Tx (TI) and Rx (RI) DMA interrupts.  The ISR
 * acks them; the test then verifies the received bytes match the transmitted
 * frame.  Prints "ENETMAC PASS" on a correct round trip through the
 * descriptor -> DMA -> loopback -> Rx descriptor -> NVIC -> handler path.
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
#define MAC_LM 0x1000u
#define DMA_SWR 0x1u
#define TX_ST 0x1u
#define RX_SR 0x1u
#define STAT_TI 0x1u
#define STAT_RI 0x40u
#define INT_TIE 0x1u
#define INT_RIE 0x40u
#define INT_NIE 0x8000u

#define TDES2_IOC 0x80000000u
#define TDES3_OWN 0x80000000u
#define TDES3_FD  0x20000000u
#define TDES3_LD  0x10000000u
#define RDES3_OWN 0x80000000u
#define RDES3_IOC 0x40000000u
#define RDES3_BUF1V 0x01000000u

/* Descriptor + buffer placement in SRAM (below the stack at 0x20010000). */
#define TXDESC 0x20008000u
#define TXBUF  0x20008100u
#define RXDESC 0x20008200u
#define RXBUF  0x20008300u
#define FRAME_LEN 64

#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MEM8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

#define NVIC_ISER4 (*(volatile uint32_t *)0xE000E110u)  /* IRQ 128..159 */
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

static volatile uint32_t tx_done, rx_done;

void enet_handler(void)
{
    uint32_t st = DMA_STATUS;
    if (st & STAT_TI) {
        tx_done = 1;
    }
    if (st & STAT_RI) {
        rx_done = 1;
    }
    DMA_STATUS = st & (STAT_TI | STAT_RI);   /* write-1-to-clear */
}

void cpu0_main(void)
{
    int i;

    LP_CTRL = CTRL_TE;
    puts_("ENETMAC test\r\n");

    /* DMA software reset (self-clears in the model). */
    DMA_MODE = DMA_SWR;
    while (DMA_MODE & DMA_SWR) {
    }

    /* Build the transmit frame: broadcast dest, a src MAC, ethertype, payload. */
    for (i = 0; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = (uint8_t)(0xA0 + i);
    }
    MEM8(TXBUF + 0) = 0xFF; MEM8(TXBUF + 1) = 0xFF; MEM8(TXBUF + 2) = 0xFF;
    MEM8(TXBUF + 3) = 0xFF; MEM8(TXBUF + 4) = 0xFF; MEM8(TXBUF + 5) = 0xFF;

    /* Tx descriptor: single buffer, first+last, owned by DMA, IOC. */
    MEM32(TXDESC + 0) = TXBUF;
    MEM32(TXDESC + 4) = 0;
    MEM32(TXDESC + 8) = TDES2_IOC | FRAME_LEN;
    MEM32(TXDESC + 12) = TDES3_OWN | TDES3_FD | TDES3_LD | FRAME_LEN;

    /* Rx descriptor: empty buffer, owned by DMA, interrupt on completion. */
    MEM32(RXDESC + 0) = RXBUF;
    MEM32(RXDESC + 4) = 0;
    MEM32(RXDESC + 8) = 0;
    MEM32(RXDESC + 12) = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;
    for (i = 0; i < FRAME_LEN; i++) {
        MEM8(RXBUF + i) = 0;
    }

    TXDESC_LIST = TXDESC; TXRING_LEN = 0;   /* one descriptor (len-1 = 0) */
    RXDESC_LIST = RXDESC; RXRING_LEN = 0;
    RX_CTRL = RX_SR;                         /* start receive DMA */
    TX_CTRL = TX_ST;                         /* start transmit DMA */
    MAC_CONFIG = MAC_TE | MAC_RE | MAC_LM;   /* enable Tx/Rx + loopback */
    DMA_INT_EN = INT_TIE | INT_RIE | INT_NIE;

    NVIC_ISER4 = (1u << (ENET_IRQ - 128));
    __asm__ volatile ("cpsie i");

    RXDESC_TAIL = RXDESC + 16;               /* publish the Rx buffer */
    TXDESC_TAIL = TXDESC + 16;               /* doorbell: transmit */

    while (!tx_done || !rx_done) {
    }

    /* Verify the received bytes match the transmitted frame. */
    int ok = 1;
    for (i = 0; i < FRAME_LEN; i++) {
        if (MEM8(RXBUF + i) != MEM8(TXBUF + i)) {
            ok = 0;
            break;
        }
    }

    puts_(ok ? "ENETMAC PASS\r\n" : "ENETMAC FAIL\r\n");
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
