/*
 * NXP MCX N USBHS1 sub-blocks — register models.
 *
 * Three register blocks of the high-speed USB1 instance:
 *   - mcxn-usbhs-phydcd  @ 0x4010A800, window 0x800: the USBHS1 PHY/DCD region
 *     (CMSIS USBHSDCD_Type, charger-detect-like; modelled as a permissive
 *     readback array sized to the full window).
 *   - mcxn-usbhs-core    @ 0x4010B000, window 0x200: the EHCI-style
 *     controller (CMSIS USBHS_Type).  USBCMD.RST self-clears, USBSTS
 *     reflects HCHalted,
 *     and the ID/HCSPARAMS/HCCPARAMS/CAPLENGTH capability registers are RO.
 *   - mcxn-usbhs-nc      @ 0x4010B200, window 0xE00: the non-core control
 *     region (CMSIS USBNC_Type; permissive readback array sized to the
 *     full window).
 *
 * Offsets/bits from the MCXN947 CMSIS header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_USBHS_H
#define HW_MISC_MCXN_USBHS_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "hw/usb/mcxn_usbdev.h"
#include "qom/object.h"

#define TYPE_MCXN_USBHS_PHYDCD "mcxn-usbhs-phydcd"
#define TYPE_MCXN_USBHS_CORE   "mcxn-usbhs-core"
#define TYPE_MCXN_USBHS_NC     "mcxn-usbhs-nc"

OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSBHSPhyDcdState, MCXN_USBHS_PHYDCD)
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSBHSCoreState, MCXN_USBHS_CORE)
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSBHSNcState, MCXN_USBHS_NC)

#define MCXN_USBHS_PHYDCD_SIZE 0x800
#define MCXN_USBHS_CORE_SIZE   0x200
#define MCXN_USBHS_NC_SIZE     0xE00

struct MCXNUSBHSPhyDcdState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t     regs[MCXN_USBHS_PHYDCD_SIZE / 4];
};

#define MCXN_USBHS_NEP     8        /* device endpoints (HWDEVICE.DEVEP)     */
#define MCXN_USBHS_MPS     512      /* high-speed bulk max packet size       */
#define MCXN_USBHS_XFERMAX 1024

/* Per-endpoint in-flight host request awaiting a firmware-primed dTD. */
typedef struct MCXNUSBHSXfer {
    bool    in_pending;
    int     in_len;
    bool    out_pending;
    int     out_len;
    uint8_t out_buf[MCXN_USBHS_XFERMAX];
} MCXNUSBHSXfer;

struct MCXNUSBHSCoreState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t     regs[MCXN_USBHS_CORE_SIZE / 4];

    MCXNUsbDevState *usbdev;        /* shared usbredir device core (link)    */
    QEMUTimer   *sof;               /* token-retry backstop tick             */
    /* deadline: the frame rate must not drift */
    int64_t     next_sof_ns;
    bool         enabled;           /* RS + device mode seen                 */
    bool         ep0_status_in;     /* drain a zero-length status-IN dTD     */
    MCXNUSBHSXfer ep[MCXN_USBHS_NEP];
};

struct MCXNUSBHSNcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/
    MemoryRegion iomem;
    uint32_t     regs[MCXN_USBHS_NC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_USBHS_H */
