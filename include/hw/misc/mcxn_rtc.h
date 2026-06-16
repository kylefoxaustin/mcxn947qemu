/*
 * NXP MCX N RTC (Real-Time Clock / calendar) — bring-up model.
 *
 * Single RTC0 instance with its own IRQ line (wired by the SoC).  The block
 * exposes 16-bit calendar/alarm/control registers plus a few 32-bit subsecond
 * and wake-timer registers.  Offsets/bits from the MCXN947 CMSIS header
 * (RTC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_RTC_H
#define HW_MISC_MCXN_RTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_RTC "mcxn-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNRTCState, MCXN_RTC)

#define MCXN_RTC_SIZE 0x1000

struct MCXNRTCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_RTC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_RTC_H */
