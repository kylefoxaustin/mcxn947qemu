/*
 * NXP MCX N CMX_PERFMON (Performance Monitor / SYSPM) — register-accurate
 * model.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (SYSPM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_CMX_PERFMON_H
#define HW_MISC_MCXN_CMX_PERFMON_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_CMX_PERFMON "mcxn-cmx-perfmon"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCMXPerfmonState, MCXN_CMX_PERFMON)

#define MCXN_CMX_PERFMON_SIZE 0x1000

struct MCXNCMXPerfmonState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_CMX_PERFMON_SIZE / 4];
};

#endif /* HW_MISC_MCXN_CMX_PERFMON_H */
