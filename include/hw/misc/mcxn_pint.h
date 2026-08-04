/*
 * NXP MCX N PINT (Pin Interrupt and Pattern Match) — functional model.
 *
 * 8 pin-interrupt channels sharing one NVIC line (PINT0_IRQn = 47).  A pin edge
 * has no source in emulation, so the 8 channel input levels are OPERATOR-DRIVEN
 * (fidelity-first, like the CMP comparator output): the "pin-input" QOM
 * property exposes what the selected pins would resolve to.  A change latches
 * the rising/falling edge-detect flags (RISE/FALL) gated by the edge enables
 * (IENR/IENF), sets the interrupt status (IST) and raises the IRQ — and,
 * for channels 0..3, pulses the corresponding eDMA request (CMSIS PINT INT0..3
 * = sources 3..6).  Offsets from the MCXN947 CMSIS header (PINT_Type).
 *
 * Edge mode (ISEL[ch]=0) is modelled; the level-sensitive mode (ISEL[ch]=1) is
 * not — the operator injects edges, which is what pin-interrupt and pin-paced
 * DMA firmware uses.  A stated boundary, not a silent one.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PINT_H
#define HW_MISC_MCXN_PINT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PINT "mcxn-pint"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPINTState, MCXN_PINT)

#define MCXN_PINT_SIZE 0x1000
#define MCXN_PINT_CHANNELS   8
/* only INT0..3 have eDMA request lines (src 3..6) */
#define MCXN_PINT_DMA_LINES  4

struct MCXNPINTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;                             /* PINT0_IRQn = 47 (shared) */
    qemu_irq dma_req[MCXN_PINT_DMA_LINES];    /* INT0..3 -> eDMA sources 3..6 */
    uint32_t regs[MCXN_PINT_SIZE / 4];

    /*
     * Operator-driven pin input: the level each of the 8 PINT channels would
     * see (post pin-mux).  Settable via the "pin-input" QOM property; a change
     * is edge-detected against this stored value.
     */
    uint8_t pin_level;
};

#endif /* HW_MISC_MCXN_PINT_H */
