/*
 * NXP MCX N OPAMP (Operational Amplifier) — register-accurate model.
 *
 * Shared device type (OPAMP_Type) instantiated three times on the MCXN947
 * (OPAMP0/1/2).  This is an analog block with no analog behaviour to emulate;
 * only the register interface is modelled.  Offsets/access-types from the
 * MCXN947 CMSIS header (OPAMP_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_OPAMP_H
#define HW_MISC_MCXN_OPAMP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_OPAMP "mcxn-opamp"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNOPAMPState, MCXN_OPAMP)

#define MCXN_OPAMP_SIZE 0x1000

struct MCXNOPAMPState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_OPAMP_SIZE / 4];
};

#endif /* HW_MISC_MCXN_OPAMP_H */
