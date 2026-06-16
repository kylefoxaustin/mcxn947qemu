/*
 * NXP MCX N OSTIMER (OS Event Timer) — functional model.
 *
 * A free-running 64-bit up-counter (presented gray-coded on EVTIMERL/H, as the
 * hardware does) with a match register (MATCH_L/H, also gray-coded); when the
 * counter reaches the match and the event is enabled (OSEVENT_CTRL.INTENA) the
 * interrupt flag is set and the OS-event IRQ raised.  Register layout from the
 * MCXN947 CMSIS header (OSTIMER_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_MCXN_OSTIMER_H
#define HW_TIMER_MCXN_OSTIMER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "hw/core/clock.h"

#define TYPE_MCXN_OSTIMER "mcxn-ostimer"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNOSTimerState, MCXN_OSTIMER)

struct MCXNOSTimerState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    QEMUTimer    timer;
    Clock       *clk;

    uint64_t base_count;   /* binary counter value at base_ns */
    int64_t  base_ns;
    uint64_t match;        /* binary match value */
    uint32_t match_gray_l, match_gray_h; /* last-written gray match halves */
    uint32_t ctrl;         /* OSEVENT_CTRL: INTRFLAG(b0) INTENA(b1) */
    uint32_t capture_l, capture_h;
};

#endif /* HW_TIMER_MCXN_OSTIMER_H */
