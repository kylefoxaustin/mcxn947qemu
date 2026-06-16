/*
 * NXP MCX N TDET (Digital Tamper, DIGTMP) — faithful register model.  See
 * source.  The status register reads the benign "no tamper" value and writes
 * never trip a tamper response, so configuring the TDET never resets the
 * machine.  Offsets/access types from the MCXN947 CMSIS header (DIGTMP_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_TDET_H
#define HW_MISC_MCXN_TDET_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_TDET "mcxn-tdet"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNTDETState, MCXN_TDET)

#define MCXN_TDET_SIZE 0x1000

struct MCXNTDETState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_TDET_SIZE / 4];
};

#endif /* HW_MISC_MCXN_TDET_H */
