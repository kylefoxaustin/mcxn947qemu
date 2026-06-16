/*
 * NXP MCX N DM (Debug Mailbox) — bring-up model.
 *
 * Faithful register file for the Debug Mailbox: CSW (command/status), REQUEST,
 * RETURN and the read-only ID register.  Offsets from the MCXN947 CMSIS header
 * (DM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_DM_H
#define HW_MISC_MCXN_DM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_DM "mcxn-dm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNDMState, MCXN_DM)

#define MCXN_DM_SIZE 0x1000

struct MCXNDMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_DM_SIZE / 4];
};

#endif /* HW_MISC_MCXN_DM_H */
