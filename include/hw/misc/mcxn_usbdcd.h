/*
 * NXP MCX N USBDCD0 — USB Device Charger Detection register model.
 *
 * Register-accurate model of the USBDCD_Type block.  CONTROL.SR (soft reset,
 * bit 25) and CONTROL.START (bit 24) self-clear so firmware sequencing
 * completes; CONTROL.IACK (bit 0) acknowledges/clears the interrupt.  Layout
 * from the MCXN947 CMSIS header (USBDCD_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_USBDCD_H
#define HW_MISC_MCXN_USBDCD_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_USBDCD "mcxn-usbdcd"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSBDCDState, MCXN_USBDCD)

#define MCXN_USBDCD_SIZE 0x1000

struct MCXNUSBDCDState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t     regs[MCXN_USBDCD_SIZE / 4];
};

#endif /* HW_MISC_MCXN_USBDCD_H */
