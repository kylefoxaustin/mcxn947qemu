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
#include "qom/object.h"

#define TYPE_MCXN_PWM "mcxn-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPWMState, MCXN_PWM)

#define MCXN_PWM_SIZE 0x1000

struct MCXNPWMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;           /* main submodule-0 capture/compare/reload line */

    uint8_t regs[MCXN_PWM_SIZE];
};

#endif /* HW_MISC_MCXN_PWM_H */
