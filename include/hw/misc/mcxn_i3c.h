/*
 * NXP MCX N I3C (Improved Inter-Integrated Circuit) — bring-up model.
 *
 * One device type covers both I3C0 and I3C1 instances.  The model presents the
 * controller (M*) and target (S*) register banks with the reset values from the
 * MCXN947 reference manual, keeps all FIFO/busy status bits in their idle state
 * so polled firmware completes, and applies write-1-to-clear to the W1C status
 * registers.  Offsets from the MCXN947 CMSIS header (I3C_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_I3C_H
#define HW_MISC_MCXN_I3C_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_I3C "mcxn-i3c"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNI3CState, MCXN_I3C)

#define MCXN_I3C_SIZE 0x1000

struct MCXNI3CState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_I3C_SIZE / 4];
};

#endif /* HW_MISC_MCXN_I3C_H */
