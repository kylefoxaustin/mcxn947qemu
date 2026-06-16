/*
 * NXP MCX N PDM / MICFIL (digital microphone interface) — bring-up model.
 *
 * Offsets/bits from the MCXN947 CMSIS header (PDM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PDM_H
#define HW_MISC_MCXN_PDM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PDM "mcxn-pdm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPDMState, MCXN_PDM)

#define MCXN_PDM_SIZE 0x1000

struct MCXNPDMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_PDM_SIZE / 4];
};

#endif /* HW_MISC_MCXN_PDM_H */
