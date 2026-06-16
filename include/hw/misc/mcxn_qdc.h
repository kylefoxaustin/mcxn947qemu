/*
 * NXP MCX N QDC (Quadrature Decoder / ENC) — bring-up model.
 *
 * Models the QDC register file with semantics sufficient for firmware to
 * initialise and read the position counters without spinning: the CTRL SWIP
 * software-trigger bit self-clears, the CTRL/CTRL2 W1C status flags clear on
 * write-1, and the position/revolution counters are plain readable storage.
 * All registers are 16-bit, so the backing store is byte addressable and
 * honours 1/2/4-byte accesses.  Register layout from the MCXN947 CMSIS header
 * (QDC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_QDC_H
#define HW_MISC_MCXN_QDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_QDC "mcxn-qdc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNQDCState, MCXN_QDC)

#define MCXN_QDC_SIZE 0x1000

struct MCXNQDCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;           /* main (compare) interrupt line */

    uint8_t regs[MCXN_QDC_SIZE];
};

#endif /* HW_MISC_MCXN_QDC_H */
