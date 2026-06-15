/*
 * NXP MCX N generic peripheral stub.
 *
 * A permissive, register-backed placeholder for a peripheral that is present
 * on the SoC but not yet functionally modelled: writes are stored, reads return
 * the stored value (so firmware config + read-back works), and nothing faults.
 * Instantiated per peripheral base from the SoC's peripheral table; each is
 * named so introspection and the unimp log identify the real block.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_STUB_H
#define HW_MISC_MCXN_STUB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_STUB "mcxn-stub"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNStubState, MCXN_STUB)

struct MCXNStubState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    char    *blkname;   /* peripheral name, for the MR label */
    uint64_t size;      /* register-window size */
    uint32_t *regs;     /* size/4 words, allocated at realize */
};

#endif /* HW_MISC_MCXN_STUB_H */
