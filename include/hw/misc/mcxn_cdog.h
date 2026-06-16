/*
 * NXP MCX N CDOG (Code Watchdog) — faithful register model.
 *
 * Models the CDOG register file without any real watchdog timeout: command
 * registers are accepted, STATUS reads back a benign state and FLAGS is
 * write-1-to-clear, so firmware that starts/stops/refreshes the watchdog
 * never trips a fault in normal operation.  Offsets/bits from the MCXN947
 * CMSIS header (CDOG_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_CDOG_H
#define HW_MISC_MCXN_CDOG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_CDOG "mcxn-cdog"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCDOGState, MCXN_CDOG)

#define MCXN_CDOG_SIZE 0x1000

struct MCXNCDOGState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_CDOG_SIZE / 4];
};

#endif /* HW_MISC_MCXN_CDOG_H */
