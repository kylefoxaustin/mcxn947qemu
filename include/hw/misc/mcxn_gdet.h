/*
 * NXP MCX N GDET (Digital Glitch Detector) — faithful register model.  See
 * source.  The register file is modelled with no active glitch/fault side
 * effect, so configuring it never trips a fault or resets the machine.
 * Offsets/access types from the MCXN947 CMSIS header (GDET_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_GDET_H
#define HW_MISC_MCXN_GDET_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_GDET "mcxn-gdet"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNGDETState, MCXN_GDET)

#define MCXN_GDET_SIZE 0x1000

struct MCXNGDETState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_GDET_SIZE / 4];
};

#endif /* HW_MISC_MCXN_GDET_H */
