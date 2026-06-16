/*
 * NXP MCX N SINC (Sinc filter) — bring-up model.
 *
 * Offsets/bits from the MCXN947 CMSIS header (SINC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SINC_H
#define HW_MISC_MCXN_SINC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_SINC "mcxn-sinc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSINCState, MCXN_SINC)

#define MCXN_SINC_SIZE 0x1000

struct MCXNSINCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_SINC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_SINC_H */
