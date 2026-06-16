/*
 * NXP MCX N AHBSC (AHB Secure Controller) — bring-up model.
 *
 * Faithful register file for the AHB secure controller: memory/peripheral
 * access-rule registers plus master-security-level registers.  Offsets from the
 * MCXN947 CMSIS header (AHBSC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_AHBSC_H
#define HW_MISC_MCXN_AHBSC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_AHBSC "mcxn-ahbsc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNAHBSCState, MCXN_AHBSC)

#define MCXN_AHBSC_SIZE 0x1000

struct MCXNAHBSCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_AHBSC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_AHBSC_H */
