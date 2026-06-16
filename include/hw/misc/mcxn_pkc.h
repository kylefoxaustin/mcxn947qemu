/*
 * NXP MCX N PKC (Public Key Crypto) - bring-up model.
 *
 * Faithful register file with no real crypto.  Firmware starts an operation
 * and polls PKC_STATUS; the model reports the engine idle (not active) so the
 * "wait for done" loops complete.  Offsets from the MCXN947 CMSIS header
 * (PKC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PKC_H
#define HW_MISC_MCXN_PKC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PKC "mcxn-pkc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPKCState, MCXN_PKC)

#define MCXN_PKC_SIZE 0x1000

struct MCXNPKCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_PKC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_PKC_H */
