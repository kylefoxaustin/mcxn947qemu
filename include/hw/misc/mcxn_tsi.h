/*
 * NXP MCX N TSI (Touch Sensing Input) — bring-up model.
 *
 * Single TSI0 instance with its own IRQ line (wired by the SoC).  Offsets/bits
 * from the MCXN947 CMSIS header (TSI_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_TSI_H
#define HW_MISC_MCXN_TSI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_TSI "mcxn-tsi"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNTSIState, MCXN_TSI)

#define MCXN_TSI_SIZE 0x1000

/* MCXN947 TSI channel count (FSL_FEATURE_TSI_CHANNEL_COUNT). */
#define MCXN_TSI_CHANNELS 25

struct MCXNTSIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_TSI_SIZE / 4];

    /* Operator-driven analog: there is no physical electrode in QEMU, so the
     * per-channel touch counter a scan would measure is exposed as a runtime
     * QOM property "tsi-countN" (the value a real electrode's capacitance would
     * drive) instead of a hidden constant.  A scan latches DATA[TSICNT] from
     * tsi_count[ CONFIG[TSICH] ].  Default = a documented sample count. */
    uint16_t tsi_count[MCXN_TSI_CHANNELS];
};

#endif /* HW_MISC_MCXN_TSI_H */
