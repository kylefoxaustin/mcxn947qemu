/*
 * NXP MCX N USBFS0 — USB Full-Speed OTG (device/host) register model.
 *
 * Register-accurate model of the USB_Type block (no transfer behaviour yet).
 * The CTL.RESET and USBTRC0.USBRESET soft-reset bits self-clear so firmware
 * reset polling completes.  Interrupt status (ISTAT/ERRSTAT/OTGISTAT) are W1C.
 * Layout/bits from the MCXN947 CMSIS header (USB_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_USBFS_H
#define HW_MISC_MCXN_USBFS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_USBFS "mcxn-usbfs"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSBFSState, MCXN_USBFS)

#define MCXN_USBFS_SIZE 0x1000

struct MCXNUSBFSState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t     regs[MCXN_USBFS_SIZE / 4];
};

#endif /* HW_MISC_MCXN_USBFS_H */
