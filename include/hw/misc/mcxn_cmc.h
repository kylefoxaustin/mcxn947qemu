/*
 * NXP MCX N CMC (Core Mode Controller) — register-accurate model.
 *
 * The CMC owns power-mode requests and exposes reset-reason status.  This is a
 * pure config/status block: it has no externally-observable behavior beyond
 * register read-back, so a register-accurate model is correct.  Offsets/bits
 * from the MCXN947 CMSIS header (CMC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_CMC_H
#define HW_MISC_MCXN_CMC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_CMC "mcxn-cmc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCMCState, MCXN_CMC)

#define MCXN_CMC_SIZE 0x1000

struct MCXNCMCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_CMC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_CMC_H */
