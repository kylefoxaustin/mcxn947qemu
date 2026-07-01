/*
 * NXP MCX N USB device-mode core — usbredir-server bridge.
 *
 * Shared, controller-agnostic core for modelling the MCXN947 USB controllers
 * (USBFS/KHCI and USBHS/ChipIdea) in *device* mode.  QEMU has no USB
 * peripheral-controller framework, so we bridge the device side to a remote USB
 * *host* over the usbredir protocol (libusbredirparser): this object plays the
 * usbredir "host/server" role (it exports a device), and the remote peer runs
 * the stock `-device usb-redir` client (e.g. an i.MX95/93 QEMU acting as host).
 *
 * The wire + standard control/enumeration plumbing lives here, ONCE.  Each
 * silicon controller backend (KHCI BDT engine / ChipIdea dQH+dTD engine) plugs
 * in via MCXNUsbBackendOps: the core turns usbredir transfers into endpoint
 * transactions the backend presents to guest firmware, and turns the firmware's
 * descriptor-driven responses back into usbredir replies.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_USB_MCXN_USBDEV_H
#define HW_USB_MCXN_USBDEV_H

#include "hw/core/qdev.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_MCXN_USBDEV "mcxn-usbdev"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUsbDevState, MCXN_USBDEV)

/*
 * Backend interface — the silicon controller engine implements these so the
 * core can drive endpoint transactions without knowing BDT vs dQH/dTD.
 *
 * All calls run on the main loop (chardev callback context).  A backend that
 * cannot complete a transaction synchronously (the usual case — guest firmware
 * must service an IRQ first) returns MCXN_USB_XFER_ASYNC and later calls
 * mcxn_usbdev_complete_in()/_out() when the descriptor it owns is retired.
 */
enum {
    MCXN_USB_XFER_OK    = 0,    /* completed synchronously                  */
    MCXN_USB_XFER_ASYNC = 1,    /* will complete later via *_complete_*()   */
    MCXN_USB_XFER_STALL = 2,    /* endpoint stalled                         */
    MCXN_USB_XFER_NAK   = 3,    /* no descriptor armed; host should retry   */
};

typedef struct MCXNUsbBackendOps {
    /* Host issued an 8-byte SETUP to EP0.  Backend latches it for firmware. */
    void (*setup)(void *be, const uint8_t setup[8]);
    /* Host wants up to @len bytes IN from @ep.  Backend fills @buf, sets
     * *@out_len, returns an MCXN_USB_XFER_* code. */
    int  (*ep_in)(void *be, int ep, uint8_t *buf, int len, int *out_len);
    /* Host sent @len OUT bytes to @ep.  Backend consumes, returns a code. */
    int  (*ep_out)(void *be, int ep, const uint8_t *buf, int len);
    /* Host set the device address / configuration (post-enumeration). */
    void (*set_address)(void *be, uint8_t addr);
    void (*set_config)(void *be, uint8_t config);
} MCXNUsbBackendOps;

/* What kind of usbredir reply a pending EP0 request completes into.  Dedicated
 * SET_CONFIGURATION / SET_INTERFACE messages must NOT be acked until firmware
 * has run their status stage — acking early lets a real importer pipeline the
 * next SETUP, which clobbers EP0 before firmware processes the previous one. */
enum {
    MCXN_USB_REPLY_XFER   = 0,  /* control_packet / bulk_packet (default)     */
    MCXN_USB_REPLY_CONFIG = 1,  /* configuration_status (arg0 = configuration)*/
    MCXN_USB_REPLY_ALT    = 2,  /* alt_setting_status (arg0=iface, arg1=alt)  */
};

/* An in-flight host request awaiting an async backend completion.  Indexed by
 * endpoint slot = (ep & 0xf) | (IN ? 0x10 : 0); EP0 control uses slot 0/16. */
typedef struct MCXNUsbPending {
    bool     active;
    bool     is_control;
    uint64_t id;                    /* usbredir transaction id               */
    uint16_t length;                /* host-requested length                 */
    uint8_t  ep;                    /* usbredir endpoint address             */
    uint8_t  reply_kind;            /* MCXN_USB_REPLY_* — how to ack          */
    uint8_t  arg0;                  /* configuration / interface             */
    uint8_t  arg1;                  /* alt setting                           */
} MCXNUsbPending;

#define MCXN_USB_NSLOTS 32

struct MCXNUsbDevState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    CharFrontend cs;                /* socket to the remote usbredir client  */
    void *parser;                   /* struct usbredirparser *               */

    /* chardev read-buffer bridge (mirrors hw/usb/redirect.c). */
    const uint8_t *read_buf;
    int            read_buf_size;
    unsigned int   watch;

    bool    connected;              /* hello handshake completed             */
    bool    attached;               /* device_connect sent (firmware enabled)*/
    uint8_t speed;                  /* usb_redir_speed_*                     */
    uint8_t cur_config;             /* current SET_CONFIGURATION value       */
    char   *gadget_profile;         /* "vendor" (default) | "cdc" — selects  */
                                    /* the interface_info/ep_info advertised */

    MCXNUsbPending pending[MCXN_USB_NSLOTS];

    /* Backend (controller engine) registration. */
    const MCXNUsbBackendOps *be_ops;
    void                    *be;
};

/* Backend registration — called by the controller engine at realize. */
void mcxn_usbdev_set_backend(MCXNUsbDevState *s,
                             const MCXNUsbBackendOps *ops, void *be);

/* The controller engine calls this when guest firmware enables the controller
 * and asserts the pull-up — i.e. when a device would appear on the bus.  Sends
 * usbredir device_connect so the remote host begins enumeration.  @speed is a
 * usb_redir_speed_* value (full/high). */
void mcxn_usbdev_attach(MCXNUsbDevState *s, uint8_t speed);

/* The controller engine calls this when firmware disables the controller /
 * drops the pull-up. */
void mcxn_usbdev_detach(MCXNUsbDevState *s);

/* Async completion hooks — backend calls these when a primed descriptor that
 * answered a previously-ASYNC host request is retired by guest firmware. */
void mcxn_usbdev_complete_in(MCXNUsbDevState *s, int ep,
                             const uint8_t *buf, int len);
/* @len is the number of bytes actually transferred (reported as the usbredir
 * actual_length — a real host driver reads it, e.g. a tty write's byte count). */
void mcxn_usbdev_complete_out(MCXNUsbDevState *s, int ep, int status, int len);

#endif /* HW_USB_MCXN_USBDEV_H */
