/*
 * NXP MCX N CACHE64_CTRL (LPCAC / cache controller) — bring-up model.
 *
 * Cache control register file (CCR/CLCR/CSAR/CCVR at offset 0x800). The CCR GO
 * bit (cache maintenance command in progress) self-clears so firmware that
 * issues an invalidate/push and polls GO never spins. Offsets/bits from the
 * MCXN947 CMSIS header (CACHE64_CTRL_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_CACHE64_CTRL_H
#define HW_MISC_MCXN_CACHE64_CTRL_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_CACHE64_CTRL "mcxn-cache64-ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCache64CtrlState, MCXN_CACHE64_CTRL)

#define MCXN_CACHE64_CTRL_SIZE 0x1000

struct MCXNCache64CtrlState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_CACHE64_CTRL_SIZE / 4];
};

#endif /* HW_MISC_MCXN_CACHE64_CTRL_H */
