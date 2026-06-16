/*
 * NXP MCX N SAI (Serial Audio Interface) — bring-up model.
 *
 * Backs the SAI/I2S transmit and receive register file (CMSIS I2S_Type) with
 * enough status semantics for firmware init to complete: soft-reset bits
 * self-clear and the FIFO flags read back consistently.  One type serves both
 * SAI0 and SAI1; the SoC supplies the per-instance base and IRQ.  Offsets/bits
 * from the MCXN947 CMSIS header (I2S_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SAI_H
#define HW_MISC_MCXN_SAI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_SAI "mcxn-sai"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSAIState, MCXN_SAI)

#define MCXN_SAI_SIZE 0x1000

struct MCXNSAIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_SAI_SIZE / 4];
};

#endif /* HW_MISC_MCXN_SAI_H */
