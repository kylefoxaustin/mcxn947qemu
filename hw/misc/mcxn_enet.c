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
#include "migration/vmstate.h"

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
    bool active = (s->regs[R_MAC_INTERRUPT_STATUS >> 2] &
                   s->regs[R_MAC_INTERRUPT_ENABLE >> 2] & MAC_IS_PHYIS) != 0;
    qemu_set_irq(s->irq, active);
}

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
    case R_MAC_INTERRUPT_STATUS:
    case R_MAC_RX_TX_STATUS:
    case R_MAC_DEBUG:
    case R_MTL_INTERRUPT_STATUS:
    case R_DMA_INTERRUPT_STATUS:
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
    qemu_set_irq(s->irq, 0);
}

static void mcxn_enet_realize(DeviceState *dev, Error **errp)
{
    MCXNEnetState *s = MCXN_ENET(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &enet_ops, s,
                          TYPE_MCXN_ENET, MCXN_ENET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_enet = {
    .name = TYPE_MCXN_ENET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNEnetState, MCXN_ENET_SIZE / 4),
        VMSTATE_UINT16_ARRAY(phy, MCXNEnetState, 32),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_enet_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_enet_realize;
    device_class_set_legacy_reset(dc, mcxn_enet_reset);
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
