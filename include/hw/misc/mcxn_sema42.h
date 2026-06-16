/*
 * NXP MCX N SEMA42 (Hardware Semaphores) — register-accurate model.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (SEMA42_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SEMA42_H
#define HW_MISC_MCXN_SEMA42_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_SEMA42 "mcxn-sema42"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSema42State, MCXN_SEMA42)

#define MCXN_SEMA42_SIZE      0x1000
#define MCXN_SEMA42_NUM_GATES 16

struct MCXNSema42State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /* Per-gate 4-bit lock state: 0 = free, n = locked by domain (n-1). */
    uint8_t gate[MCXN_SEMA42_NUM_GATES];
    /* Reset-gate read-back state (RSTGT_R). */
    uint16_t rstgt;
};

#endif /* HW_MISC_MCXN_SEMA42_H */
