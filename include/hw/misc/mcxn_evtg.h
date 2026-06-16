/*
 * NXP MCX N EVTG (Event Generator) — bring-up model.
 *
 * Routing/configuration block: 4 EVTG instances (0x10 bytes each), each with
 * AOI boolean-function-term config, a control/status register and output
 * filters.  All registers are __IO configuration fields that reset to 0 with
 * no externally-observable behavior in emulation.  Offsets from the MCXN947
 * CMSIS header (EVTG_Type, base 0x400D2000).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_EVTG_H
#define HW_MISC_MCXN_EVTG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_EVTG "mcxn-evtg"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNEvtgState, MCXN_EVTG)

#define MCXN_EVTG_SIZE 0x1000

struct MCXNEvtgState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_EVTG_SIZE / 4];
};

#endif /* HW_MISC_MCXN_EVTG_H */
