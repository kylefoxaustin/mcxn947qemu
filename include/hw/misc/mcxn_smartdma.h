/*
 * NXP MCX N SMARTDMA (programmable DMA coprocessor) - bring-up model.
 *
 * The SMARTDMA is an "EZH" programmable engine started via the CTRL.START bit.
 * This model backs the control/boot registers permissively and keeps the engine
 * reading not-busy so firmware that boots it and polls never stalls.  Offsets
 * and bits from the MCXN947 CMSIS header (SMARTDMA_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SMARTDMA_H
#define HW_MISC_MCXN_SMARTDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_SMARTDMA "mcxn-smartdma"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSmartDMAState, MCXN_SMARTDMA)

#define MCXN_SMARTDMA_SIZE 0x1000

struct MCXNSmartDMAState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_SMARTDMA_SIZE / 4];
};

#endif /* HW_MISC_MCXN_SMARTDMA_H */
