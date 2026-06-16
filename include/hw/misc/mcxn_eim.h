/*
 * NXP MCX N EIM (Error Injection Module) — register-accurate model.
 *
 * The EIM injects RAM ECC errors for fault testing.  This is a pure config
 * block: every register is RW (CMSIS __IO) and resets to 0, and there is no
 * externally-observable behavior in emulation, so register read-back is
 * faithful.  Offsets/bits from the MCXN947 CMSIS header (EIM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_EIM_H
#define HW_MISC_MCXN_EIM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_EIM "mcxn-eim"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNEIMState, MCXN_EIM)

#define MCXN_EIM_SIZE 0x1000

struct MCXNEIMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_EIM_SIZE / 4];
};

#endif /* HW_MISC_MCXN_EIM_H */
