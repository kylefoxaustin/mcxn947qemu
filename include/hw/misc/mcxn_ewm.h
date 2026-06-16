/*
 * NXP MCX N EWM (External Watchdog Monitor) — faithful register model.
 *
 * Models the EWM register file (CTRL/SERV/CMPL/CMPH/CLKCTRL/CLKPRESCALER)
 * with no real watchdog timeout, so firmware that configures or services the
 * EWM never trips ewm_out_b.  CMPH resets to 0xFF per the RM.  Offsets/bits
 * from the MCXN947 CMSIS header (EWM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_EWM_H
#define HW_MISC_MCXN_EWM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_EWM "mcxn-ewm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNEWMState, MCXN_EWM)

#define MCXN_EWM_SIZE 0x1000

struct MCXNEWMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint8_t ctrl;
    uint8_t cmpl;
    uint8_t cmph;
    uint8_t clkctrl;
    uint8_t clkprescaler;
};

#endif /* HW_MISC_MCXN_EWM_H */
