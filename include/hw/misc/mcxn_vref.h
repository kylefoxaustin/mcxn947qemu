/*
 * NXP MCX N VREF (Voltage Reference) — register-accurate model.
 *
 * Single instance on the MCXN947 (VREF0).  This is an analog block with no
 * analog behaviour to emulate; only the register interface is modelled.
 * Offsets/access-types from the MCXN947 CMSIS header (VREF_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_VREF_H
#define HW_MISC_MCXN_VREF_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_VREF "mcxn-vref"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNVREFState, MCXN_VREF)

#define MCXN_VREF_SIZE 0x1000

struct MCXNVREFState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_VREF_SIZE / 4];
};

#endif /* HW_MISC_MCXN_VREF_H */
