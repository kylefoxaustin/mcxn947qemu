/*
 * NXP MCX N PLU (Programmable Logic Unit) — bring-up model.
 *
 * Configuration block: 26 LUTs (input mux + truth tables), an output mux and
 * a wake/interrupt control register.  All registers are __IO config that reset
 * to 0, except OUTPUTS (offset 0x900) which is __I read-only and reflects the
 * combinatorial LUT outputs (reset 0).  Offsets from the MCXN947 CMSIS header
 * (PLU_Type, base 0x40034000).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PLU_H
#define HW_MISC_MCXN_PLU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PLU "mcxn-plu"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPluState, MCXN_PLU)

#define MCXN_PLU_SIZE 0x1000

struct MCXNPluState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_PLU_SIZE / 4];
};

#endif /* HW_MISC_MCXN_PLU_H */
