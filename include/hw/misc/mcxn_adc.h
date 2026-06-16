/*
 * NXP MCX N ADC (LPADC, 16-bit SAR with result FIFO) — bring-up model.
 *
 * One shared type covers ADC0 and ADC1; both instances share an identical
 * register layout in the MCXN947 CMSIS header (ADC_Type).  Each instance has
 * its own IRQ line, wired by the SoC.  Offsets/bits from ADC_Type.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_ADC_H
#define HW_MISC_MCXN_ADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_ADC "mcxn-adc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNADCState, MCXN_ADC)

#define MCXN_ADC_SIZE 0x1000

struct MCXNADCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_ADC_SIZE / 4];

    /* Modelled result FIFO 0: a single completed conversion is presented when
     * software arms a conversion (SWTRIG / TCTRL).  fifo_valid means RESFIFO[0]
     * holds an unread result. */
    uint32_t fifo_data;
    bool fifo_valid;
};

#endif /* HW_MISC_MCXN_ADC_H */
