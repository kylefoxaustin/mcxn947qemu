/*
 * NXP MCX N ENET (Ethernet QoS, Synopsys DesignWare) — bring-up model.
 *
 * The bring-up blockers are the driver init handshakes:
 *
 *   - Software reset: firmware sets DMA_MODE.SWR (bit 0) and polls until the
 *     hardware clears it.  The RM states SWR "is automatically cleared after the
 *     reset operation is complete".  Here the reset is instantaneous: SWR reads
 *     back 0 and the register file is reset.
 *   - MAC enable: MAC_CONFIGURATION.RE/TE (receive/transmit enable) simply read
 *     back what firmware wrote.
 *   - MDIO (PHY management): firmware programs MAC_MDIO_ADDRESS and sets the GB
 *     (busy) bit to kick a PHY read/write, then polls GB until it clears.  Here
 *     the access completes instantaneously: GB reads back 0, the MAC interrupt
 *     status PHYIS event is raised, and a Management Read returns 0xFFFF in
 *     MAC_MDIO_DATA (no PHY present -> all-ones, link down).
 *
 * MAC_INTERRUPT_STATUS / MAC_RX_TX_STATUS and the DMA/MTL status registers read
 * idle.  MAC_VERSION / MAC_HW_FEAT report read-only constants.  All other
 * registers are backed permissively by regs[].
 *
 * Offsets/bits from the MCXN947 CMSIS header (ENET_Type); semantics from the
 * Ethernet QoS chapter of the reference manual.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_enet.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/dma.h"
#include "system/address-spaces.h"
#include "net/eth.h"
#include "qemu/module.h"

/* Register offsets (ENET_Type). */
#define R_MAC_CONFIGURATION    0x000
#define R_MAC_INTERRUPT_STATUS 0x0B0   /* RO */
#define R_MAC_INTERRUPT_ENABLE 0x0B4
#define R_MAC_RX_TX_STATUS     0x0B8   /* RO */
#define R_MAC_VERSION          0x110   /* RO */
#define R_MAC_DEBUG            0x114   /* RO */
#define R_MAC_HW_FEAT0         0x11C   /* RO (HW_FEAT0..3 @ 0x11C..0x128) */
#define R_MAC_MDIO_ADDRESS     0x200
#define R_MAC_MDIO_DATA        0x204
#define R_MTL_INTERRUPT_STATUS 0xC20   /* RO */
#define R_DMA_MODE             0x1000
#define R_DMA_INTERRUPT_STATUS 0x1008  /* RO */
#define R_DMA_DEBUG_STATUS0    0x100C  /* RO */

/* MAC_MDIO_ADDRESS fields (DWC ENET QoS, Clause 22). */
#define MDIO_GB          (1u << 0)   /* operation busy (self-clears when done) */
#define MDIO_GOC_SHIFT   2           /* GOC[1:0]: 0b01 write, 0b11 read */
#define MDIO_GOC_MASK    0x3u
#define MDIO_GOC_WRITE   0x1u
#define MDIO_GOC_READ    0x3u
#define MDIO_RDA_SHIFT   16          /* register/device address */
#define MDIO_RDA_MASK    0x1Fu
#define MDIO_PA_SHIFT    21          /* physical (PHY) address */
#define MDIO_PA_MASK     0x1Fu

/* MAC_INTERRUPT_STATUS/ENABLE.PHYIS/PHYIE: PHY-event interrupt (bit 3). */
#define MAC_IS_PHYIS     (1u << 3)
#define MAC_IE_PHYIE     (1u << 3)
/* DMA_MODE.SWR: software reset (self-clears when reset completes). */
#define DMA_MODE_SWR     (1u << 0)

/* MAC_CONFIGURATION enables. */
#define MAC_CFG_RE       (1u << 0)   /* receiver enable    */
#define MAC_CFG_TE       (1u << 1)   /* transmitter enable */
#define MAC_CFG_LM       (1u << 12)  /* MAC loopback mode  */

/* DMA channel-0 register offsets (DWC ENET-QoS). */
#define R_DMA_CH0_TX_CTRL       0x1104
#define R_DMA_CH0_RX_CTRL       0x1108
#define R_DMA_CH0_TXDESC_LIST   0x1114
#define R_DMA_CH0_RXDESC_LIST   0x111C
#define R_DMA_CH0_TXDESC_TAIL   0x1120
#define R_DMA_CH0_RXDESC_TAIL   0x1128
#define R_DMA_CH0_TXRING_LEN    0x112C
#define R_DMA_CH0_RXRING_LEN    0x1130
#define R_DMA_CH0_INT_EN        0x1134
#define R_DMA_CH0_STATUS        0x1160
#define R_MTL_TXQ0_OP_MODE      0x0D00  /* MTL Tx queue-0 operation mode */
#define MTL_TXQ_FTQ      (1u << 0)   /* flush Tx queue (self-clearing) */

/* DMA channel control / status / interrupt-enable bits. */
#define DMA_TX_ST        (1u << 0)   /* start transmission */
#define DMA_RX_SR        (1u << 0)   /* start receive      */
#define DMA_STAT_TI      (1u << 0)   /* transmit interrupt */
#define DMA_STAT_RI      (1u << 6)   /* receive interrupt  */
#define DMA_STAT_RBU     (1u << 7)   /* receive buffer unavailable */
#define DMA_STAT_NIS     (1u << 15)  /* normal interrupt summary   */
#define DMA_INT_TIE      (1u << 0)
#define DMA_INT_RIE      (1u << 6)
#define DMA_INT_NIE      (1u << 15)

/* Descriptor bits (DWC ENET-QoS normal descriptors). */
#define TDES2_IOC        (1u << 31)  /* interrupt on completion */
#define TDES3_OWN        (1u << 31)
#define TDES3_FD         (1u << 29)  /* first descriptor */
#define TDES3_LD         (1u << 28)  /* last descriptor  */
#define TDES2_B1L_MASK   0x3FFFu     /* buffer-1 length  */
#define RDES3_OWN        (1u << 31)
#define RDES3_FD         (1u << 29)
#define RDES3_LD         (1u << 28)
#define RDES3_PL_MASK    0x7FFFu     /* packet length (write-back) */

#define ENET_FRAME_MAX   2048
#define ENET_RING_GUARD  256         /* bound the descriptor walk */

/*
 * Model PHY: a single Clause-22 PHY answering at MDIO address 2 (a common
 * default for the FRDM-MCXN947 RMII PHY).  It reports link-up with
 * auto-negotiation complete so a driver's "wait for link" loop terminates.
 * The PHY identity is a plausible Microchip LAN8741-class value (model data,
 * not silicon-verified).
 */
#define ENET_PHY_ADDR    2
#define PHY_BMCR         0x00
#define PHY_BMSR         0x01
#define PHY_ID1          0x02
#define PHY_ID2          0x03
#define PHY_BMCR_RESET   0x1140u  /* AN enable, 100M, full-duplex */
#define PHY_BMSR_VALUE   0x782Du  /* 10/100 capable, AN able+complete, link up */
#define PHY_ID1_VALUE    0x0007u
#define PHY_ID2_VALUE    0xC110u

/*
 * Read-only identification constants.  The RM does not document an explicit
 * MAC_VERSION reset value; 0x51 (DesignWare ENET QoS core v5.10, NXP user
 * version 0) is the conventional value for this IP generation.  GUESSED — to be
 * confirmed against silicon / the SDK if a driver checks it.
 */
#define MAC_VERSION_VALUE  0x00000051u

static void mcxn_enet_update_irq(MCXNEnetState *s)
{
    bool mac = (s->regs[R_MAC_INTERRUPT_STATUS >> 2] &
                s->regs[R_MAC_INTERRUPT_ENABLE >> 2] & MAC_IS_PHYIS) != 0;
    bool dma = (s->regs[R_DMA_CH0_STATUS >> 2] &
                s->regs[R_DMA_CH0_INT_EN >> 2] &
                (DMA_STAT_TI | DMA_STAT_RI)) != 0;

    qemu_set_irq(s->irq, mac || dma);
}

/* Read/write a 16-byte little-endian descriptor at a guest address. */
static void enet_desc_read(uint32_t addr, uint32_t d[4])
{
    dma_memory_read(&address_space_memory, addr, d, 16, MEMTXATTRS_UNSPECIFIED);
    for (int i = 0; i < 4; i++) {
        d[i] = le32_to_cpu(d[i]);
    }
}

static void enet_desc_write_word3(uint32_t addr, uint32_t w3)
{
    uint32_t le = cpu_to_le32(w3);
    dma_memory_write(&address_space_memory, addr + 12, &le, 4,
                     MEMTXATTRS_UNSPECIFIED);
}

/*
 * Deliver a received frame into the Rx descriptor ring.  The receive buffer
 * size (RBSZ, from DMA_CH0_RX_CTRL) is typically smaller than a frame, so the
 * frame is split across consecutive owned descriptors: each holds up to RBSZ
 * bytes and its control word carries the CUMULATIVE length through that buffer
 * (what the driver expects); the first descriptor is flagged FD, the last LD.
 */
static bool mcxn_enet_deliver(MCXNEnetState *s, const uint8_t *buf, size_t len)
{
    uint32_t base = s->regs[R_DMA_CH0_RXDESC_LIST >> 2] & ~0x3u;
    uint32_t ringlen = (s->regs[R_DMA_CH0_RXRING_LEN >> 2] & 0x3FF) + 1;
    uint32_t rbsz = (s->regs[R_DMA_CH0_RX_CTRL >> 2] >> 1) & 0x3FFF;
    size_t off = 0;
    bool first = true;

    if (!(s->regs[R_MAC_CONFIGURATION >> 2] & MAC_CFG_RE) ||
        !(s->regs[R_DMA_CH0_RX_CTRL >> 2] & DMA_RX_SR) || base == 0) {
        return false;
    }
    if (rbsz == 0 || rbsz > ENET_FRAME_MAX) {
        rbsz = ENET_FRAME_MAX;
    }
    if (len > ENET_FRAME_MAX) {
        len = ENET_FRAME_MAX;
    }

    do {
        uint32_t d[4], chunk;
        bool last;

        enet_desc_read(s->cur_rx, d);
        if (!(d[3] & RDES3_OWN)) {
            s->regs[R_DMA_CH0_STATUS >> 2] |= DMA_STAT_RBU | DMA_STAT_NIS;
            mcxn_enet_update_irq(s);
            return false;   /* ran out of buffers */
        }
        chunk = MIN(rbsz, len - off);
        dma_memory_write(&address_space_memory, d[0], buf + off, chunk,
                         MEMTXATTRS_UNSPECIFIED);
        off += chunk;
        last = (off >= len);
        /* Write-back: clear OWN, FD on first / LD on last, cumulative length. */
        enet_desc_write_word3(s->cur_rx,
                              (first ? RDES3_FD : 0) | (last ? RDES3_LD : 0) |
                              ((uint32_t)off & RDES3_PL_MASK));
        first = false;

        s->cur_rx += 16;
        if (s->cur_rx >= base + ringlen * 16) {
            s->cur_rx = base;
        }
    } while (off < len);

    s->regs[R_DMA_CH0_STATUS >> 2] |= DMA_STAT_RI | DMA_STAT_NIS;
    mcxn_enet_update_irq(s);
    return true;
}

/* Walk the Tx ring, assembling and sending each owned, completed frame. */
static void mcxn_enet_tx_process(MCXNEnetState *s)
{
    uint32_t base = s->regs[R_DMA_CH0_TXDESC_LIST >> 2] & ~0x3u;
    uint32_t tail = s->regs[R_DMA_CH0_TXDESC_TAIL >> 2] & ~0x3u;
    uint32_t ringlen = (s->regs[R_DMA_CH0_TXRING_LEN >> 2] & 0x3FF) + 1;
    bool loopback = s->regs[R_MAC_CONFIGURATION >> 2] & MAC_CFG_LM;
    uint8_t frame[ENET_FRAME_MAX];
    uint32_t flen = 0;
    int guard = ENET_RING_GUARD;
    bool ioc = false;

    if (!(s->regs[R_MAC_CONFIGURATION >> 2] & MAC_CFG_TE) ||
        !(s->regs[R_DMA_CH0_TX_CTRL >> 2] & DMA_TX_ST) || base == 0) {
        return;
    }

    while (s->cur_tx != tail && guard-- > 0) {
        uint32_t d[4], b1len, b2len;

        enet_desc_read(s->cur_tx, d);
        if (!(d[3] & TDES3_OWN)) {
            break;   /* descriptor still owned by software */
        }
        if (d[3] & TDES3_FD) {
            flen = 0;
            ioc = false;
        }
        /* TDES2 carries buffer-1 length [13:0] and buffer-2 length [29:16];
         * the driver puts the L2 header in buffer 1 and the payload in
         * buffer 2 (TDES0 / TDES1 addresses). */
        b1len = d[2] & TDES2_B1L_MASK;
        if (b1len && flen + b1len <= sizeof(frame)) {
            dma_memory_read(&address_space_memory, d[0], frame + flen, b1len,
                            MEMTXATTRS_UNSPECIFIED);
            flen += b1len;
        }
        b2len = (d[2] >> 16) & TDES2_B1L_MASK;
        if (b2len && flen + b2len <= sizeof(frame)) {
            dma_memory_read(&address_space_memory, d[1], frame + flen, b2len,
                            MEMTXATTRS_UNSPECIFIED);
            flen += b2len;
        }
        if (d[2] & TDES2_IOC) {
            ioc = true;
        }
        enet_desc_write_word3(s->cur_tx, d[3] & ~TDES3_OWN);   /* give to CPU */

        if (d[3] & TDES3_LD) {
            if (loopback) {
                mcxn_enet_deliver(s, frame, flen);
            } else if (s->nic) {
                qemu_send_packet(qemu_get_queue(s->nic), frame, flen);
            }
            if (ioc) {
                s->regs[R_DMA_CH0_STATUS >> 2] |= DMA_STAT_TI | DMA_STAT_NIS;
            }
            flen = 0;
        }

        s->cur_tx += 16;
        if (s->cur_tx >= base + ringlen * 16) {
            s->cur_tx = base;
        }
    }
    mcxn_enet_update_irq(s);
}

static bool mcxn_enet_can_receive(NetClientState *nc)
{
    MCXNEnetState *s = qemu_get_nic_opaque(nc);
    return (s->regs[R_MAC_CONFIGURATION >> 2] & MAC_CFG_RE) &&
           (s->regs[R_DMA_CH0_RX_CTRL >> 2] & DMA_RX_SR);
}

static ssize_t mcxn_enet_receive(NetClientState *nc, const uint8_t *buf,
                                 size_t size)
{
    MCXNEnetState *s = qemu_get_nic_opaque(nc);

    mcxn_enet_deliver(s, buf, size);
    return size;
}

static NetClientInfo net_mcxn_enet_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mcxn_enet_can_receive,
    .receive = mcxn_enet_receive,
};

/* Read a Clause-22 PHY register; BMSR always reports live link-up status. */
static uint16_t mcxn_enet_phy_read(MCXNEnetState *s, unsigned reg)
{
    if (reg == PHY_BMSR) {
        return PHY_BMSR_VALUE;
    }
    return s->phy[reg & 0x1F];
}

static bool enet_reg_ro(hwaddr off)
{
    switch (off) {
    case R_MAC_RX_TX_STATUS:
    case R_MAC_VERSION:
    case R_MAC_DEBUG:
    case R_MAC_HW_FEAT0:
    case R_MAC_HW_FEAT0 + 4:
    case R_MAC_HW_FEAT0 + 8:
    case R_MAC_HW_FEAT0 + 12:
    case R_MTL_INTERRUPT_STATUS:
    case R_DMA_INTERRUPT_STATUS:
    case R_DMA_DEBUG_STATUS0:
        return true;
    default:
        return false;
    }
}

static uint64_t enet_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNEnetState *s = MCXN_ENET(opaque);
    uint32_t shift = (off & 3) * 8;
    uint32_t v;

    if (off >= MCXN_ENET_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off & ~3u) {
    case R_MAC_VERSION:
        v = MAC_VERSION_VALUE;
        break;
    case R_DMA_INTERRUPT_STATUS:
        /* DC0IS (bit 0) summarises DMA channel 0's pending interrupt; the
         * driver's ISR gates on it before reading the channel status. */
        v = (s->regs[R_DMA_CH0_STATUS >> 2] & s->regs[R_DMA_CH0_INT_EN >> 2] &
             (DMA_STAT_TI | DMA_STAT_RI | DMA_STAT_RBU)) ? 1u : 0u;
        break;
    case R_MAC_INTERRUPT_STATUS:
    case R_MAC_RX_TX_STATUS:
    case R_MAC_DEBUG:
    case R_MTL_INTERRUPT_STATUS:
    case R_DMA_DEBUG_STATUS0:
        /* Status registers read idle. */
        v = s->regs[(off & ~3u) >> 2];
        break;
    default:
        v = s->regs[(off & ~3u) >> 2];
        break;
    }

    return (v >> shift) & ((size == 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1));
}

static void enet_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    MCXNEnetState *s = MCXN_ENET(opaque);
    uint32_t idx = (off & ~3u) >> 2;
    uint32_t shift = (off & 3) * 8;
    uint32_t mask = (size == 4) ? 0xFFFFFFFFu
                                : (((1u << (size * 8)) - 1) << shift);
    uint32_t val;

    if (off >= MCXN_ENET_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    if (enet_reg_ro(off & ~3u)) {
        return;   /* read-only identification/status registers */
    }

    val = (s->regs[idx] & ~mask) | ((uint32_t)(value << shift) & mask);

    switch (off & ~3u) {
    case R_DMA_MODE:
        /* SWR self-clears: the soft reset is instantaneous in the model. */
        s->regs[idx] = val & ~DMA_MODE_SWR;
        return;
    case R_MTL_TXQ0_OP_MODE:
        /* FTQ (flush Tx queue) self-clears: the flush completes instantly. */
        s->regs[idx] = val & ~MTL_TXQ_FTQ;
        return;
    case R_MAC_MDIO_ADDRESS:
        /*
         * Kicking a PHY management op (GB=1) completes immediately: GB clears,
         * the access hits the model PHY at ENET_PHY_ADDR (other addresses read
         * all-ones = no device), and the PHYIS event is raised.
         */
        if (val & MDIO_GB) {
            uint32_t pa = (val >> MDIO_PA_SHIFT) & MDIO_PA_MASK;
            uint32_t rda = (val >> MDIO_RDA_SHIFT) & MDIO_RDA_MASK;
            uint32_t goc = (val >> MDIO_GOC_SHIFT) & MDIO_GOC_MASK;

            val &= ~MDIO_GB;
            if (pa == ENET_PHY_ADDR) {
                if (goc == MDIO_GOC_WRITE) {
                    s->phy[rda] = s->regs[R_MAC_MDIO_DATA >> 2] & 0xFFFFu;
                } else {
                    s->regs[R_MAC_MDIO_DATA >> 2] =
                        (s->regs[R_MAC_MDIO_DATA >> 2] & 0xFFFF0000u) |
                        mcxn_enet_phy_read(s, rda);
                }
            } else {
                s->regs[R_MAC_MDIO_DATA >> 2] =
                    (s->regs[R_MAC_MDIO_DATA >> 2] & 0xFFFF0000u) | 0xFFFFu;
            }
            s->regs[R_MAC_INTERRUPT_STATUS >> 2] |= MAC_IS_PHYIS;
            mcxn_enet_update_irq(s);
        }
        s->regs[idx] = val;
        return;
    case R_MAC_INTERRUPT_STATUS:
        /* Event bits are write-1-to-clear in the model so an ISR can ack. */
        s->regs[idx] &= ~((uint32_t)(value << shift) & mask);
        mcxn_enet_update_irq(s);
        return;
    case R_MAC_INTERRUPT_ENABLE:
        s->regs[idx] = val;
        mcxn_enet_update_irq(s);
        return;
    case R_DMA_CH0_TXDESC_LIST:
        s->regs[idx] = val;
        s->cur_tx = val & ~0x3u;
        return;
    case R_DMA_CH0_RXDESC_LIST:
        s->regs[idx] = val;
        s->cur_rx = val & ~0x3u;
        return;
    case R_DMA_CH0_RXDESC_TAIL:
        /* Publishing Rx buffers makes the receiver ready: unblock the net
         * queue so any frame held while can_receive was false is delivered. */
        s->regs[idx] = val;
        if (s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    case R_DMA_CH0_TXDESC_TAIL:
        /* Tail-pointer write is the transmit doorbell. */
        s->regs[idx] = val;
        mcxn_enet_tx_process(s);
        return;
    case R_DMA_CH0_STATUS:
        /* Interrupt-status bits are write-1-to-clear. */
        s->regs[idx] &= ~val;
        mcxn_enet_update_irq(s);
        return;
    case R_DMA_CH0_INT_EN:
        s->regs[idx] = val;
        mcxn_enet_update_irq(s);
        return;
    case R_MAC_CONFIGURATION:
    case R_DMA_CH0_RX_CTRL:
        /* Enabling the receiver unblocks any queued inbound frame. */
        s->regs[idx] = val;
        if (s->nic && (s->regs[R_MAC_CONFIGURATION >> 2] & MAC_CFG_RE) &&
            (s->regs[R_DMA_CH0_RX_CTRL >> 2] & DMA_RX_SR)) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    default:
        s->regs[idx] = val;
        return;
    }
}

static const MemoryRegionOps enet_ops = {
    .read = enet_read,
    .write = enet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_enet_reset(DeviceState *dev)
{
    MCXNEnetState *s = MCXN_ENET(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->phy, 0, sizeof(s->phy));
    s->phy[PHY_BMCR] = PHY_BMCR_RESET;
    s->phy[PHY_BMSR] = PHY_BMSR_VALUE;
    s->phy[PHY_ID1]  = PHY_ID1_VALUE;
    s->phy[PHY_ID2]  = PHY_ID2_VALUE;
    s->cur_tx = 0;
    s->cur_rx = 0;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_enet_realize(DeviceState *dev, Error **errp)
{
    MCXNEnetState *s = MCXN_ENET(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &enet_ops, s,
                          TYPE_MCXN_ENET, MCXN_ENET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_mcxn_enet_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const VMStateDescription vmstate_mcxn_enet = {
    .name = TYPE_MCXN_ENET,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNEnetState, MCXN_ENET_SIZE / 4),
        VMSTATE_UINT16_ARRAY(phy, MCXNEnetState, 32),
        VMSTATE_UINT32(cur_tx, MCXNEnetState),
        VMSTATE_UINT32(cur_rx, MCXNEnetState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mcxn_enet_properties[] = {
    DEFINE_NIC_PROPERTIES(MCXNEnetState, conf),
};

static void mcxn_enet_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_enet_realize;
    device_class_set_legacy_reset(dc, mcxn_enet_reset);
    device_class_set_props(dc, mcxn_enet_properties);
    dc->vmsd = &vmstate_mcxn_enet;
}

static const TypeInfo mcxn_enet_types[] = {
    {
        .name          = TYPE_MCXN_ENET,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEnetState),
        .class_init    = mcxn_enet_class_init,
    },
};

DEFINE_TYPES(mcxn_enet_types)
