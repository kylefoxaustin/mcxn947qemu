/*
 * NXP MCX N EMVSIM (EMV smartcard interface) — bring-up model.
 *
 * One shared type for EMVSIM0/EMVSIM1.  Offsets/bits from the MCXN947 CMSIS
 * header (EMVSIM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_EMVSIM_H
#define HW_MISC_MCXN_EMVSIM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_EMVSIM "mcxn-emvsim"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNEMVSIMState, MCXN_EMVSIM)

#define MCXN_EMVSIM_SIZE 0x1000

struct MCXNEMVSIMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t regs[MCXN_EMVSIM_SIZE / 4];
};

#endif /* HW_MISC_MCXN_EMVSIM_H */
