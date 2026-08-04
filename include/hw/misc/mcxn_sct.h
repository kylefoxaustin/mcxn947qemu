/*
 * NXP MCX N SCT (SCTimer/PWM) — bring-up model.
 *
 * Models the SCT register file with semantics sufficient for firmware to
 * configure and start the timer without spinning: the CONFIG/CTRL bits reflect
 * back, the CTRL CLRCTR_L/CLRCTR_H self-clearing counter-clear bits self-clear,
 * the EVFLAG/CONFLAG event/conflict flags are write-1-to-clear, and the COUNT
 * register is plain readable storage.  Registers are a mix of 32-bit and paired
 * 16-bit halves, so the backing store is byte addressable and honours
 * 1/2/4-byte accesses.  Register layout from the MCXN947 CMSIS header
 * (SCT_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SCT_H
#define HW_MISC_MCXN_SCT_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_MCXN_SCT "mcxn-sct"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSCTState, MCXN_SCT)

#define MCXN_SCT_SIZE 0x1000

struct MCXNSCTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    Clock       *clk;   /* driven by SYSCON[SCTCLKSEL]/[SCTCLKDIV] */
    qemu_irq     irq;           /* SCT0_IRQn */
    /* SCT0 DMA0 / DMA1 request (pulse on a selected event) */
    qemu_irq     dma_req[2];
    QEMUTimer    event_timer;
    /* deadline: periodic timers must not drift */
    /* periodic match/limit event 0 */
    int64_t next_event_ns;

    /*
     * Operator-driven SCT input pin levels (AIN0..7), for input-conditioned
     * events: an edge that matches an event's IOCOND/IOSEL fires that event.
     * The pins have no signal source in emulation, so they are driven via the
     * "sct-inputs" QOM property.
     */
    uint8_t in_level;

    uint8_t regs[MCXN_SCT_SIZE];
};

#endif /* HW_MISC_MCXN_SCT_H */
