/*
 * NXP MCX N DAC (12-bit DAC with output FIFO) — bring-up model.
 *
 * One shared type covers DAC0/DAC1 (LPDAC) and DAC2 (HPDAC); the three share
 * an identical register layout in the MCXN947 CMSIS header.  Offsets/bits from
 * LPDAC_Type/HPDAC_Type.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_DAC_H
#define HW_MISC_MCXN_DAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_DAC "mcxn-dac"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNDACState, MCXN_DAC)

#define MCXN_DAC_SIZE 0x1000

struct MCXNDACState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_DAC_SIZE / 4];
    uint32_t data;   /* last value written to DATA (the modelled "output") */
};

#endif /* HW_MISC_MCXN_DAC_H */
