/*
 * MCXN947 ENET (Ethernet QoS) MDIO + PHY + NVIC interrupt test.
 *
 * Performs the driver bring-up handshakes against the model: a DMA software
 * reset (self-clearing), then MDIO reads of the model PHY at address 2 -
 * verifying the PHY identity (PHY_ID1) and a link-up / autoneg-complete BMSR.
 * Then enables the PHY-event interrupt (MAC PHYIE) and the ENET NVIC line
 * (IRQ 139) and kicks one more MDIO read, which raises PHYIS and delivers the
 * interrupt; the ISR acks it.  Prints "ENET PASS" on a correct PHY read plus a
 * delivered interrupt through the MDIO -> PHYIS -> NVIC -> handler path.
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
#define MAC_INT_STATUS (*(volatile uint32_t *)(ENET + 0x0B0))
#define MAC_INT_ENABLE (*(volatile uint32_t *)(ENET + 0x0B4))
#define MAC_MDIO_ADDR  (*(volatile uint32_t *)(ENET + 0x200))
#define MAC_MDIO_DATA  (*(volatile uint32_t *)(ENET + 0x204))
#define DMA_MODE       (*(volatile uint32_t *)(ENET + 0x1000))

#define MDIO_GB     (1u << 0)
#define MDIO_READ   (0x3u << 2)
#define DMA_SWR     (1u << 0)
#define MAC_PHYIS   (1u << 3)
#define MAC_PHYIE   (1u << 3)

#define PHY_ADDR 2
#define PHY_BMSR 1
#define PHY_ID1  2
#define PHY_ID1_EXPECT 0x0007u
#define BMSR_LINK (1u << 2)

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

static uint32_t mdio_read(uint32_t pa, uint32_t rda)
{
    MAC_MDIO_ADDR = (pa << 21) | (rda << 16) | MDIO_READ | MDIO_GB;
    while (MAC_MDIO_ADDR & MDIO_GB) {   /* GB self-clears when the op completes */
    }
    return MAC_MDIO_DATA & 0xFFFFu;
}

static volatile uint32_t irq_count;

void enet_handler(void)
{
    MAC_INT_STATUS = MAC_PHYIS;   /* write-1-to-clear PHYIS */
    irq_count++;
}

void cpu0_main(void)
{
    uint32_t phy_id, bmsr;

    LP_CTRL = CTRL_TE;
    puts_("ENET test\r\n");

    /* DMA software reset: self-clears. */
    DMA_MODE = DMA_SWR;
    while (DMA_MODE & DMA_SWR) {
    }

    phy_id = mdio_read(PHY_ADDR, PHY_ID1);
    bmsr   = mdio_read(PHY_ADDR, PHY_BMSR);

    /* Clear any pending PHYIS, then arm the interrupt path. */
    MAC_INT_STATUS = MAC_PHYIS;
    MAC_INT_ENABLE = MAC_PHYIE;
    NVIC_ISER4 = (1u << (ENET_IRQ - 128));
    __asm__ volatile ("cpsie i");

    /* One more MDIO access raises PHYIS -> delivers the interrupt. */
    (void)mdio_read(PHY_ADDR, PHY_BMSR);

    while (irq_count < 1) {
    }

    if (phy_id == PHY_ID1_EXPECT && (bmsr & BMSR_LINK) && irq_count >= 1) {
        puts_("ENET PASS\r\n");
    } else {
        puts_("ENET FAIL\r\n");
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
