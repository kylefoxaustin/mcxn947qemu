/*
 * NXP MCX N WWDT (Windowed Watchdog Timer) — bring-up model.
 *
 * Models the windowed watchdog register file (MOD, TC, FEED, TV, WARNINT,
 * WINDOW) without ever triggering a watchdog reset.  One shared type, two
 * instances (WWDT0, WWDT1).  Offsets/reset values from the MCXN947 CMSIS header
 * (WWDT_Type) and the reference manual.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_WWDT_H
#define HW_MISC_MCXN_WWDT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_WWDT "mcxn-wwdt"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNWWDTState, MCXN_WWDT)

#define MCXN_WWDT_SIZE 0x1000

struct MCXNWWDTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_WWDT_SIZE / 4];
};

#endif /* HW_MISC_MCXN_WWDT_H */
