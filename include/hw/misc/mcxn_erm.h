/*
 * NXP MCX N ERM (Error Recovery Manager) — register-accurate model.
 *
 * The ERM reports RAM ECC errors (correctable/non-correctable) flagged by the
 * memory controllers.  This is a pure status block: no error is ever injected
 * in emulation, so error status reads "no error".  Offsets/bits/access-types
 * from the MCXN947 CMSIS header (ERM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_ERM_H
#define HW_MISC_MCXN_ERM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_ERM "mcxn-erm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNERMState, MCXN_ERM)

#define MCXN_ERM_SIZE 0x1000

struct MCXNERMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_ERM_SIZE / 4];
};

#endif /* HW_MISC_MCXN_ERM_H */
