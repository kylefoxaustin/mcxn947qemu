/*
 * NXP MCX N WUU (Wake-Up Unit) — bring-up model.
 *
 * Pin/module wake-up enable register file. VERID/PARAM read constant; the pin
 * flag (PF) register is write-1-to-clear and reads benign (no pending wakes).
 * Offsets/bits from the MCXN947 CMSIS header (WUU_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_WUU_H
#define HW_MISC_MCXN_WUU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_WUU "mcxn-wuu"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNWUUState, MCXN_WUU)

#define MCXN_WUU_SIZE 0x1000

struct MCXNWUUState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_WUU_SIZE / 4];
};

#endif /* HW_MISC_MCXN_WUU_H */
