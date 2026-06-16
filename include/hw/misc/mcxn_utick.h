/*
 * NXP MCX N UTICK (Micro-Tick Timer) — bring-up model.
 *
 * Models the micro-tick timer register file: CTRL, the W1C STAT register, CFG,
 * the write-only CAPCLR and the read-only CAP[4] capture registers.  Offsets
 * from the MCXN947 CMSIS header (UTICK_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_UTICK_H
#define HW_MISC_MCXN_UTICK_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_UTICK "mcxn-utick"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUTICKState, MCXN_UTICK)

#define MCXN_UTICK_SIZE 0x1000

struct MCXNUTICKState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_UTICK_SIZE / 4];
};

#endif /* HW_MISC_MCXN_UTICK_H */
