/*
 * NXP MCX N SPC (System Power Controller) — bring-up model.
 *
 * Enough of the SPC for firmware power/voltage init to complete: the SRAMCTL
 * REQ->ACK handshake is acknowledged instantly and the SC.BUSY status reads
 * idle.  Remaining registers are backed permissively.  Offsets/bits from the
 * MCXN947 CMSIS header (SPC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SPC_H
#define HW_MISC_MCXN_SPC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_SPC "mcxn-spc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSPCState, MCXN_SPC)

#define MCXN_SPC_SIZE 0x1000

struct MCXNSPCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_SPC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_SPC_H */
