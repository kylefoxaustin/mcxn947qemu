/*
 * NXP MCX N CTIMER (Standard counter/timer) — functional core.
 *
 * Models the timer counter (TC) with prescale (PR/PC), the four match registers
 * (MR0..3) and their per-match interrupt / reset / stop actions (MCR), and the
 * interrupt flags (IR) driving the NVIC line.  INPUT CAPTURE on channel 0 is
 * functional: an operator-driven capture-input edge (CCR-selected) latches the
 * live counter into CR0 and raises IR[CR0INT].  External match (EMR) and PWM
 * (PWMC/MSR) registers are stored but not yet functional.  Register layout from
 * the MCXN947 CMSIS header (CTIMER_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_MCXN_CTIMER_H
#define HW_TIMER_MCXN_CTIMER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "hw/core/clock.h"

#define TYPE_MCXN_CTIMER "mcxn-ctimer"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNCTimerState, MCXN_CTIMER)

struct MCXNCTimerState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    /* match-0 / match-1 eDMA request (pulse per match) */
    qemu_irq     dma_req[2];
    /* per-match trigger output (INPUTMUX -> ADCn_TRIG) */
    qemu_irq     match_trig[4];
    QEMUTimer    timer;
    Clock       *clk;

    uint32_t ir;          /* interrupt flags (W1C)            */
    uint32_t tcr;         /* control: CEN (b0), CRST (b1)     */
    uint32_t pr;          /* prescale max                     */
    uint32_t mcr;         /* match control (3 bits per match) */
    uint32_t mr[4];       /* match values                     */
    uint32_t ccr, emr, ctcr, pwmc;   /* capture/ext-match/count-ctrl/PWM */
    uint32_t cr[4], msr[4];          /* capture / match-shadow (stored)  */

    /* Counter state, valid as of base_ns. */
    uint32_t tc;          /* timer counter      */
    uint32_t pc;          /* prescale counter   */
    int64_t  base_ns;     /* when tc/pc were last synced */

    /*
     * Operator-driven capture input 0 (the CAP0 pin level), for input capture:
     * a CCR-selected edge latches the live TC into CR0.  The pin has no signal
     * source in emulation, so it is driven via the "capture-input" QOM
     * property.
     */
    bool cap0_level;
};

#endif /* HW_TIMER_MCXN_CTIMER_H */
