/*
 * NXP MCX N OTPC (OTP / fuse controller) — bring-up model.
 *
 * Models the OTP controller register file. The SR busy/in-progress bits read
 * "done" so firmware that triggers a read/reload and polls SR never spins.
 * Fuse read data (RDATA) reads 0. VERID/PARAM read constant. Offsets/bits from
 * the MCXN947 CMSIS header (OTPC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_OTPC_H
#define HW_MISC_MCXN_OTPC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_OTPC "mcxn-otpc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNOTPCState, MCXN_OTPC)

#define MCXN_OTPC_SIZE 0x1000

struct MCXNOTPCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_OTPC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_OTPC_H */
