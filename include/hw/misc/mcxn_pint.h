/*
 * NXP MCX N PINT (Pin Interrupt and Pattern Match) — bring-up model.
 *
 * Models the pin-interrupt enable/status register file: ISEL, the rising/falling
 * enable registers with their set/clear aliases, RISE/FALL detection and the
 * W1C IST status register, plus the pattern-match registers.  Offsets from the
 * MCXN947 CMSIS header (PINT_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PINT_H
#define HW_MISC_MCXN_PINT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PINT "mcxn-pint"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPINTState, MCXN_PINT)

#define MCXN_PINT_SIZE 0x1000

struct MCXNPINTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_PINT_SIZE / 4];
};

#endif /* HW_MISC_MCXN_PINT_H */
