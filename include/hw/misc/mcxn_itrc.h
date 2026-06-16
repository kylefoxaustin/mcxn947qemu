/*
 * NXP MCX N ITRC (Intrusion and Tamper Response Controller) — faithful
 * register model.  See source.  Status registers read the benign "no event"
 * value and writes never trip a tamper response, so configuring the ITRC
 * never resets the machine.  Offsets/access types from the MCXN947 CMSIS
 * header (ITRC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_ITRC_H
#define HW_MISC_MCXN_ITRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_ITRC "mcxn-itrc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNITRCState, MCXN_ITRC)

#define MCXN_ITRC_SIZE 0x1000

struct MCXNITRCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_ITRC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_ITRC_H */
