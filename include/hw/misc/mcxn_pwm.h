/*
 * NXP MCX N eFlexPWM (PWM) — bring-up model.
 *
 * Models the eFlexPWM register file (four submodules SM[0..3] plus the shared
 * top-level registers) with semantics sufficient for firmware to initialise and
 * run without spinning: the MCTRL RUN bits reflect back, LDOK self-clears after
 * a load request, CLDOK clears LDOK, and the per-submodule STS flags are
 * write-1-to-clear.  Most registers are 16-bit, so the backing store is byte
 * addressable and honours 1/2/4-byte accesses.  Register layout from the
 * MCXN947 CMSIS header (PWM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PWM_H
#define HW_MISC_MCXN_PWM_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_MCXN_PWM "mcxn-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPWMState, MCXN_PWM)

#define MCXN_PWM_SIZE 0x1000

struct MCXNPWMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    /* IPBus/bus clock — derived from the SCG main clock */
    Clock       *clk;
    /* main submodule-0 capture/compare/reload line */
    qemu_irq     irq;
    /* submodule-0 value-register DMA request (reload-driven) */
    qemu_irq     dma_req_val;
    /* current level of dma_req_val, so we edge-detect */
    bool         val_dma_lvl;
    /* submodule-0 input-A capture DMA request (pulse per edge) */
    qemu_irq     dma_req_capa;
    /* operator-driven input-A pin level (for edge-detect) */
    bool         capa_level;
    /* FlexPWM FAULT interrupt line (NVIC FLEXPWMn_FAULT) */
    qemu_irq     irq_fault;
    /* operator-driven FAULT0 input pin level (for edge-detect) */
    bool         fault_level;
    QEMUTimer    reload_timer;  /* submodule-0 periodic reload */
    /* the reload DEADLINE, so the carrier cannot drift */
    int64_t      next_reload_ns;
    /* submodule-0 PWM_OUT_TRIG0/1 -> INPUTMUX -> ADC trigger */
    qemu_irq     out_trig[2];
    /* fires at the next enabled VALn output-trigger compare */
    QEMUTimer    trig_timer;
    /* deadline of the pending output-trigger compare */
    int64_t      next_trig_ns;
    /* which OUT_TRIG lines fire at next_trig_ns (bit0/bit1) */
    uint8_t      trig_mask;

    uint8_t regs[MCXN_PWM_SIZE];
};

#endif /* HW_MISC_MCXN_PWM_H */
