/*
 * NXP MCX N USBDCD0 — USB Device Charger Detection.
 *
 * Runs the BC1.2 detection SEQUENCE (data-pin contact -> primary
 * SDP-vs-charging -> secondary CDP-vs-DCP), stepping through
 * STATUS[SEQ_STAT]/[SEQ_RES] with an interrupt per phase, exactly as
 * the USBDCD_Type block does.  What is attached to the port has no physical
 * existence in QEMU, so the CLASSIFICATION is operator-driven (the `charger`
 * QOM property / SIGNAL_OVERRIDE), never fabricated -- the model runs the
 * protocol and reports the port the operator wired to it.
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

/* Operator-selected attached port (the `charger` property). */
enum {
    MCXN_DCD_NONE = 0,   /* nothing attached -> data-pin contact times out */
    MCXN_DCD_SDP  = 1,   /* Standard Downstream Port (a USB host)          */
    MCXN_DCD_CDP  = 2,   /* Charging Downstream Port                       */
    MCXN_DCD_DCP  = 3,   /* Dedicated Charging Port                        */
};

struct MCXNUSBDCDState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t     regs[MCXN_USBDCD_SIZE / 4];

    /* operator property: what is attached (MCXN_DCD_*) */
    uint8_t      charger;
    uint8_t      phase;      /* BC1.2 sequence phase (0 = idle) */
};

#endif /* HW_MISC_MCXN_USBDCD_H */
