/*
 * NXP MCX N CRC (Cyclic Redundancy Check) engine.
 *
 * Functional CRC-16/CRC-32 data-path model: DATA writes feed the engine
 * (seed when CTRL[WAS]=1, data otherwise) and reads of DATA return the
 * computed checksum.  Offsets/bits from the MCXN947 CMSIS header (CRC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_CRC_H
#define HW_MISC_MCXN_CRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_CRC "mcxn-crc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCRCState, MCXN_CRC)

#define MCXN_CRC_SIZE 0x1000

struct MCXNCRCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t data;     /* current CRC accumulator / seed */
    uint32_t gpoly;    /* polynomial */
    uint32_t ctrl;     /* control */
};

#endif /* HW_MISC_MCXN_CRC_H */
