/*
 * NXP MCX N MRT (Multi-Rate Timer) — functional model.
 *
 * Four independent 31-bit down-counter channels: writing INTVAL loads the
 * counter, it counts down at the bus clock, and reaching zero sets the
 * channel's interrupt flag (STAT.INTFLAG) and, when enabled (CTRL.INTEN),
 * raises the shared MRT IRQ.  Repeat mode reloads INTVAL; one-shot stops.
 * Register layout from the MCXN947 CMSIS header (MRT_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_MCXN_MRT_H
#define HW_TIMER_MCXN_MRT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "hw/core/clock.h"

#define TYPE_MCXN_MRT "mcxn-mrt"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNMRTState, MCXN_MRT)

#define MCXN_MRT_CHANNELS 4

struct MCXNMRTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    QEMUTimer    timer;
    Clock       *clk;

    uint32_t intval[MCXN_MRT_CHANNELS]; /* reload value (bits 0..30)   */
    uint32_t ctrl[MCXN_MRT_CHANNELS];   /* INTEN (b0), MODE (b1..2)    */
    uint32_t stat[MCXN_MRT_CHANNELS];   /* INTFLAG (b0,W1C), RUN (b1)  */
    uint32_t load[MCXN_MRT_CHANNELS];   /* down-count value at base_ns */
    int64_t  base_ns[MCXN_MRT_CHANNELS];
};

#endif /* HW_TIMER_MCXN_MRT_H */
