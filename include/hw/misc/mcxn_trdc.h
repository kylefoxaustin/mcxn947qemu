/*
 * NXP MCX N TRDC (Trusted Resource Domain Controller) - bring-up model.
 *
 * Faithful access-control register file with no access enforcement.  The MBC
 * domain/memory configuration registers are backed permissively so firmware
 * can program domain access policy and read it back.  Offsets from the MCXN947
 * CMSIS header (TRDC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_TRDC_H
#define HW_MISC_MCXN_TRDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_TRDC "mcxn-trdc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNTRDCState, MCXN_TRDC)

#define MCXN_TRDC_SIZE 0x1000

struct MCXNTRDCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_TRDC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_TRDC_H */
