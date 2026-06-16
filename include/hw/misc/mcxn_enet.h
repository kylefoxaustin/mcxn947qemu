/*
 * NXP MCX N ENET (Ethernet QoS, Synopsys DesignWare) — bring-up model.
 *
 * Models the MAC/MTL/DMA control registers accurately enough that firmware's
 * software-reset, MAC-enable and MDIO (PHY management) accesses complete so the
 * driver init loop terminates.  The full mapped window is backed by a flat
 * regs[] array.  Offsets/bits from the MCXN947 CMSIS header (ENET_Type);
 * semantics from the Ethernet QoS chapter of the RM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_ENET_H
#define HW_MISC_MCXN_ENET_H

#include "hw/core/sysbus.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_MCXN_ENET "mcxn-enet"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNEnetState, MCXN_ENET)

/*
 * ENET_Type spans the MAC block (offset 0x0) through the DMA channel array
 * (DMA_CH[2] at 0x1100, step 0x80, ending at 0x1200).  The mapped window is
 * rounded up to the next power of two, 0x2000.
 */
#define MCXN_ENET_SIZE 0x2000

struct MCXNEnetState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;
    uint32_t cur_tx;    /* current Tx descriptor address (DMA channel 0) */
    uint32_t cur_rx;    /* current Rx descriptor address (DMA channel 0) */
    uint32_t regs[MCXN_ENET_SIZE / 4];
    uint16_t phy[32];   /* model Clause-22 PHY register file (one PHY) */
};

#endif /* HW_MISC_MCXN_ENET_H */
