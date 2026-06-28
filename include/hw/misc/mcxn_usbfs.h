/*
 * NXP MCX N USBFS0 — USB Full-Speed (KHCI) controller, device mode.
 *
 * Register-accurate KHCI model plus a *device-mode* endpoint engine: the
 * Buffer-Descriptor-Table (BDT) ping-pong banks firmware arms in RAM are read
 * and retired here, and host transactions arrive from a remote USB host via the
 * shared usbredir core (hw/usb/mcxn_usbdev.c).  We present SETUP/IN/OUT tokens
 * to guest firmware (TOKDNE + STAT) and relay the firmware's descriptor-driven
 * responses back to the host.  Layout/bits from the MCXN947 CMSIS header
 * (USB_Type / PERI_USB.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_USBFS_H
#define HW_MISC_MCXN_USBFS_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "hw/usb/mcxn_usbdev.h"
#include "qom/object.h"

#define TYPE_MCXN_USBFS "mcxn-usbfs"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSBFSState, MCXN_USBFS)

#define MCXN_USBFS_SIZE 0x1000
#define MCXN_USBFS_NEP  16          /* ENDPT0..15                            */
#define MCXN_USBFS_MPS  64          /* full-speed max packet size            */

/* Per-endpoint in-flight host request awaiting firmware-armed BD(s).  A single
 * host transfer may span several max-packet BDs, so IN data is accumulated and
 * OUT data is drained across ping-pong banks until the transfer completes (a
 * short/zero packet or the host-requested length is reached). */
#define MCXN_USBFS_XFERMAX 1024
typedef struct MCXNUSBFSXfer {
    bool    in_pending;             /* host wants IN data from this ep       */
    int     in_len;                 /* host-requested length                 */
    int     in_acc;                 /* bytes accumulated so far              */
    uint8_t in_buf[MCXN_USBFS_XFERMAX];
    bool    out_pending;            /* host has OUT data for this ep         */
    int     out_len;                /* total OUT length                      */
    int     out_off;                /* bytes delivered to firmware so far    */
    uint8_t out_buf[MCXN_USBFS_XFERMAX];
} MCXNUSBFSXfer;

struct MCXNUSBFSState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    uint32_t     regs[MCXN_USBFS_SIZE / 4];

    MCXNUsbDevState *usbdev;        /* shared usbredir device core (link)    */
    QEMUTimer   *sof;               /* 1 ms SOF / token-retry tick           */

    bool        enabled;            /* CTL.USBENSOFEN seen                   */
    bool        tokdne_busy;        /* a TOKDNE is awaiting firmware ack     */
    uint8_t     odd_rx[MCXN_USBFS_NEP];  /* next ping-pong bank, OUT/RX      */
    uint8_t     odd_tx[MCXN_USBFS_NEP];  /* next ping-pong bank, IN/TX       */

    bool        setup_pending;      /* a SETUP awaits delivery to EP0 RX     */
    uint8_t     setup_pkt[8];

    MCXNUSBFSXfer ep[MCXN_USBFS_NEP];
};

#endif /* HW_MISC_MCXN_USBFS_H */
