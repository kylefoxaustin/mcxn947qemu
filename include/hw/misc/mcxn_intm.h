/*
 * NXP MCX N INTM (Interrupt Monitor) — register-accurate model.
 *
 * The INTM monitors interrupt-request-to-acknowledge latency for up to four
 * monitors.  This is a pure config/status block with no externally-observable
 * behavior in emulation, so a register-accurate model is correct.
 * Offsets/bits/access-types from the MCXN947 CMSIS header (INTM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_INTM_H
#define HW_MISC_MCXN_INTM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_INTM "mcxn-intm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNINTMState, MCXN_INTM)

#define MCXN_INTM_SIZE 0x1000

struct MCXNINTMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_INTM_SIZE / 4];
};

#endif /* HW_MISC_MCXN_INTM_H */
