/*
 * NXP MCX N CMP (Low-Power Analog Comparator) — register-accurate model.
 *
 * Shared device type (CMP_Type / CMSIS LPCMP_Type) instantiated three times on
 * the MCXN947 (CMP0/1/2).  This is an analog block with no analog behaviour to
 * emulate; only the register interface is modelled.  Offsets/access-types from
 * the MCXN947 CMSIS header (LPCMP_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_CMP_H
#define HW_MISC_MCXN_CMP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_CMP "mcxn-cmp"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCMPState, MCXN_CMP)

#define MCXN_CMP_SIZE 0x1000

struct MCXNCMPState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_CMP_SIZE / 4];
};

#endif /* HW_MISC_MCXN_CMP_H */
