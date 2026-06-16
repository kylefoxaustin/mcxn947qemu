/*
 * NXP MCX N INPUTMUX (input multiplexer / trigger routing) — bring-up model.
 *
 * Pure register-routing block: selects which signals drive peripheral
 * triggers, capture inputs, DMA requests, etc.  Faithful register-backed
 * model (all registers reset to 0).  Routing has no externally-observable
 * effect in emulation.  Offsets from the MCXN947 CMSIS header (INPUTMUX_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_INPUTMUX_H
#define HW_MISC_MCXN_INPUTMUX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_INPUTMUX "mcxn-inputmux"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNInputMuxState, MCXN_INPUTMUX)

#define MCXN_INPUTMUX_SIZE 0x1000

struct MCXNInputMuxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_INPUTMUX_SIZE / 4];
};

#endif /* HW_MISC_MCXN_INPUTMUX_H */
