/*
 * NXP MCX N USBPHY — USB 2.0 high-speed integrated PHY register model.
 *
 * Register-accurate model of the USBPHY_Type block (window 0x800).  Implements
 * the SET/CLR/TOG alias semantics (base+4 sets, base+8 clears, base+C toggles).
 * CTRL.SFTRST self-clears and CTRL.CLKGATE clears so the PHY reads ungated;
 * VERSION is read-only.  Layout/bits from the MCXN947 CMSIS header
 * (USBPHY_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_USBPHY_H
#define HW_MISC_MCXN_USBPHY_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_USBPHY "mcxn-usbphy"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSBPHYState, MCXN_USBPHY)

#define MCXN_USBPHY_SIZE 0x800

struct MCXNUSBPHYState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t     regs[MCXN_USBPHY_SIZE / 4];
};

#endif /* HW_MISC_MCXN_USBPHY_H */
