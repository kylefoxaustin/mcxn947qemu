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
#define MCXN_ADC_CHANNELS 16   /* operator-settable analog inputs ch0..15 */

/* RM ADC chapter: "Supports 16-word depth FIFO with the configurable watermark."
 * Two FIFOs (0 and 1); TCTRL[FIFO_SEL_A] picks the destination for a conversion. */
#define MCXN_ADC_NFIFO      2
#define MCXN_ADC_FIFO_DEPTH 16

struct MCXNADCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    /* DMA request lines into the eDMA: ADC{n} FIFO A/B = mux sources 21+2n / 22+2n.
     * Without these the stock LPADC EDMA driver waits forever. */
    qemu_irq dma_req[2];
    bool     dma_req_level[2];
    uint32_t regs[MCXN_ADC_SIZE / 4];

    /* Operator-driven analog inputs: the conversion result is the value of the
     * channel selected by the triggered command, NOT a hidden constant.  Each
     * is a runtime QOM property "adc-chN" (the model's analog of the board pin
     * voltage); default = documented mid-scale.  Inject via:
     *   qom-set /machine/.../adc0 adc-ch5 2748 */
    uint16_t adc_ch[MCXN_ADC_CHANNELS];

    /*
     * The two result FIFOs, each MCXN_ADC_FIFO_DEPTH entries deep.
     *
     * ⚠ THIS USED TO BE `uint32_t fifo_data; bool fifo_valid;` -- A SINGLE SLOT.
     * A second conversion arriving before the first was drained OVERWROTE it,
     * silently: no flag, no error, a conversion simply ceased to exist.  Real
     * silicon has a 16-word FIFO and raises STAT[FOFn] on overflow, and it drops
     * the NEW result, not the old one (RM: "The newer data is not stored and the
     * FIFO holds the original contents").
     *
     * A depth-1 "FIFO" is not a simplification of a depth-16 one.  It SILENTLY
     * DISABLES THE FEATURE BUILT ON TOP OF IT: FCTRL[FWMARK] is a watermark, and
     * a watermark on a one-deep queue is meaningless -- occupancy can only be 0
     * or 1, so any firmware asking to be woken at FWMARK >= 1 (i.e. "tell me when
     * 8 samples are ready", the entire point of the register) waited forever.
     * The model had all the watermark plumbing and nothing to watermark.
     */
    uint32_t fifo[MCXN_ADC_NFIFO][MCXN_ADC_FIFO_DEPTH];
    uint8_t  fifo_count[MCXN_ADC_NFIFO];
};

#endif /* HW_MISC_MCXN_ADC_H */
