/*
 * NXP MCX N TSI (Touch Sensing Input) — bring-up model.
 *
 * Single TSI0 instance with its own IRQ line (wired by the SoC).  Offsets/bits
 * from the MCXN947 CMSIS header (TSI_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_TSI_H
#define HW_MISC_MCXN_TSI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_TSI "mcxn-tsi"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNTSIState, MCXN_TSI)

#define MCXN_TSI_SIZE 0x1000

struct MCXNTSIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_TSI_SIZE / 4];
};

#endif /* HW_MISC_MCXN_TSI_H */
