/*
 * MCXN947 FlexCAN loopback + NVIC interrupt test.
 *
 * Puts CAN0 in loopback mode (CTRL1.LPB), arms RX message buffer 4 to receive,
 * unmasks its interrupt (IMASK1 bit 4) and the CAN0 NVIC line (IRQ 62), then
 * arms TX message buffer 0 with a data frame (CS CODE=0xC).  The model loops
 * the frame back into MB4, sets CODE=FULL, copies the ID/data, sets IFLAG1
 * bit 4 and asserts the IRQ.  The ISR reads MB4 back and verifies the payload.
 * Prints "FLEXCAN PASS" on a correct round trip through the full
 * TX -> loopback -> RX MB -> IFLAG -> NVIC -> handler path.
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

#define CTRL1_LPB     (1u << 12)
#define CODE_RX_EMPTY (0x4u << 24)
#define CODE_TX_DATA  (0xCu << 24)
#define CODE_RX_FULL  0x2u
#define DLC8          (8u << 16)

#define RX_MB 4
#define TX_MB 0
#define CAN_ID  0x14550000u
#define DATA0   0xDEADBEEFu
#define DATA1   0x12345678u

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 enable */
#define CAN0_IRQ 62

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

static volatile uint32_t got;
static volatile uint32_t rx_code, rx_id, rx_d0, rx_d1;

void can0_handler(void)
{
    if (CAN_IFLAG1 & (1u << RX_MB)) {
        rx_code = (MB_CS(RX_MB) >> 24) & 0xF;
        rx_id = MB_ID(RX_MB);
        rx_d0 = MB_W0(RX_MB);
        rx_d1 = MB_W1(RX_MB);
        CAN_IFLAG1 = (1u << RX_MB);   /* write-1-to-clear */
        got = 1;
    }
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("FLEXCAN test\r\n");

    CAN_MCR = 0;                 /* enable module, leave freeze, ready */
    CAN_CTRL1 = CTRL1_LPB;       /* internal loopback */
    CAN_RXMGMASK = 0;            /* accept any ID */

    /* Arm RX MB4. */
    MB_ID(RX_MB) = CAN_ID;
    MB_CS(RX_MB) = CODE_RX_EMPTY;

    CAN_IMASK1 = (1u << RX_MB);
    NVIC_ISER1 = (1u << (CAN0_IRQ - 32));
    __asm__ volatile ("cpsie i");

    /* Arm TX MB0 with a data frame: writing CS (CODE=TX_DATA) transmits. */
    MB_W0(TX_MB) = DATA0;
    MB_W1(TX_MB) = DATA1;
    MB_ID(TX_MB) = CAN_ID;
    MB_CS(TX_MB) = CODE_TX_DATA | DLC8;

    while (!got) {
    }

    if (rx_code == CODE_RX_FULL && rx_id == CAN_ID &&
        rx_d0 == DATA0 && rx_d1 == DATA1) {
        puts_("FLEXCAN PASS\r\n");
    } else {
        puts_("FLEXCAN FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + CAN0_IRQ] = can0_handler,  /* exception 78 = IRQ 62 */
};
