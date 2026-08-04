/*
 * NXP MCX N LPTMR (Low-Power Timer) — functional model.
 *
 * Time-counter mode: the counter (CNR) counts up at the prescaled clock; when
 * it reaches the compare value (CMR) the compare flag (CSR.TCF) is set and,
 * when enabled (CSR.TIE), the IRQ is raised; the counter then restarts (period
 * = CMR + 1).  Register layout from the MCXN947 CMSIS header (LPTMR_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_MCXN_LPTMR_H
#define HW_TIMER_MCXN_LPTMR_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "hw/core/clock.h"

#define TYPE_MCXN_LPTMR "mcxn-lptmr"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNLPTMRState, MCXN_LPTMR)

struct MCXNLPTMRState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    /*
     * LPTMR's compare match is not only an interrupt -- it is a TRIGGER OUTPUT,
     * routed through INPUTMUX to the ADC/DAC/CMP.  "Convert on a timer
     * tick" is THE canonical embedded pattern, and the stock lpadc/edma
     * example is built on it.  Without this line the timer fires, the
     * trigger goes nowhere, and the ADC never converts -- silently.
     */
    qemu_irq     trigger;
    QEMUTimer    timer;
    Clock       *clk;

    uint32_t csr;       /* TEN(b0) TMS(b1) TFC(b2) TIE(b6) TCF(b7) */
    uint32_t psr;       /* prescaler / clock select               */
    uint32_t cmr;       /* compare value                          */
    uint32_t cnr;       /* counter value at base_ns               */
    int64_t  base_ns;
};

#endif /* HW_TIMER_MCXN_LPTMR_H */
