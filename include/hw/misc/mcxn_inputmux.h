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

/* DMA0/DMA1, 128 request sources each (DMAn_REQ_ENABLE0..3, 32 bits apiece). */
#define MCXN_INPUTMUX_NDMA 2
#define MCXN_INPUTMUX_NREQ 128

struct MCXNInputMuxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_INPUTMUX_SIZE / 4];

    /*
     * One output line per eDMA request source, per eDMA.  These carry
     * DMAn_REQ_ENABLE0..3 -- the gate that decides whether a peripheral's DMA
     * request is allowed to reach the engine at all.
     *
     * ⚠ Until now this file's own comment said INPUTMUX registers "have no
     * externally-observable behavior in emulation, so storing and reading back the
     * written value is faithful."  THAT JUSTIFICATION IS CIRCULAR: they had no
     * observable behaviour ONLY BECAUSE NOTHING WAS CONNECTED TO THE OTHER END.
     * The stub asserted its own irrelevance, and the assertion was self-fulfilling.
     */
    qemu_irq dma_req_enable[MCXN_INPUTMUX_NDMA][MCXN_INPUTMUX_NREQ];
};

#endif /* HW_MISC_MCXN_INPUTMUX_H */
