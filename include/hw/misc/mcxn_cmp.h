/*
 * NXP MCX N CMP (Low-Power Analog Comparator) — register-accurate model.
 *
 * Shared device type (CMP_Type / CMSIS LPCMP_Type) instantiated three times on
 * the MCXN947 (CMP0/1/2).  QEMU has no analog stimulus, so the comparator
 * output is OPERATOR-DRIVEN: the level a real +/- input pair would resolve to
 * is exposed as the "comparator-output" QOM property.  Toggling it latches the
 * rising/falling edge flags (CSR[CFR]/CSR[CFF]) and raises the comparator IRQ
 * when enabled.  Offsets/access-types from the MCXN947 CMSIS header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_CMP_H
#define HW_MISC_MCXN_CMP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_CMP "mcxn-cmp"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCMPState, MCXN_CMP)

#define MCXN_CMP_SIZE 0x1000

struct MCXNCMPState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    /* CMP DMA request: an IER-enabled edge, redirected by CCR1[DMA_EN] */
    qemu_irq dma_req;
    uint32_t regs[MCXN_CMP_SIZE / 4];

    /*
     * Operator-driven analog: the comparator output level (CSR[COUT]) the
     * +/- inputs would resolve to.  Settable via the "comparator-output" QOM
     * property; a transition latches CSR[CFR] (rising) / CSR[CFF] (falling).
     */
    bool cout;

    /*
     * Round-robin baseline: the channel-0 output captured when RR_EN was set
     * (the RR_INITMOD "expected" state).  A triggered round-robin sample that
     * deviates from it sets CSR[RRF].
     */
    bool rr_baseline;
};

#endif /* HW_MISC_MCXN_CMP_H */
