/*
 * NXP MCX N NPX0 / eIQ Neutron NPU (neural accelerator) - bring-up model.
 *
 * The window at 0x400C_C000 is named NPX0 in the MCXN947 CMSIS header, but the
 * documented NPX_Type there describes a flash-cache obfuscation block, not the
 * Neutron NPU compute register set (which is not broken out in CMSIS).  This
 * model is therefore a permissive 0x1000 register array with readback, plus
 * firmware-safe status semantics: any soft-reset request self-clears, any
 * busy/run status reads idle, and the interrupt-status word is
 * write-1-to-clear.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_NPU_H
#define HW_MISC_MCXN_NPU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_NPU "mcxn-npu"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNNPUState, MCXN_NPU)

#define MCXN_NPU_SIZE 0x1000

struct MCXNNPUState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_NPU_SIZE / 4];
};

#endif /* HW_MISC_MCXN_NPU_H */
