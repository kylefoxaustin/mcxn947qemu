/*
 * NXP MCX N POWERQUAD (DSP math coprocessor) - bring-up model.
 *
 * Firmware loads operands, writes CONTROL to launch a math instruction, then
 * polls CONTROL.INST_BUSY (and/or INTRSTAT) for completion.  This model treats
 * every instruction as completing instantly: CONTROL.INST_BUSY always reads
 * not-busy.  Offsets and bits from the MCXN947 CMSIS header (POWERQUAD_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_POWERQUAD_H
#define HW_MISC_MCXN_POWERQUAD_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_POWERQUAD "mcxn-powerquad"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPowerQuadState, MCXN_POWERQUAD)

#define MCXN_POWERQUAD_SIZE 0x1000

struct MCXNPowerQuadState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_POWERQUAD_SIZE / 4];
};

#endif /* HW_MISC_MCXN_POWERQUAD_H */
