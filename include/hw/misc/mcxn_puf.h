/*
 * NXP MCX N PUF (Physically Unclonable Function) - bring-up model.
 *
 * Faithful register file with no real PUF.  Firmware issues enroll/start/
 * get-key commands via CR and polls SR; the model reports the engine idle and
 * the last operation OK so those loops complete.  Offsets from the MCXN947
 * CMSIS header (PUF_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PUF_H
#define HW_MISC_MCXN_PUF_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PUF "mcxn-puf"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPUFState, MCXN_PUF)

#define MCXN_PUF_SIZE 0x1000

struct MCXNPUFState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_PUF_SIZE / 4];
};

#endif /* HW_MISC_MCXN_PUF_H */
