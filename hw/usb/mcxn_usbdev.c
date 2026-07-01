/*
 * NXP MCX N USB device-mode core — usbredir-server bridge.
 *
 * See include/hw/usb/mcxn_usbdev.h for the design.  This file owns the usbredir
 * wire (libusbredirparser) and the chardev bridge; the controller backends
 * (KHCI/ChipIdea) register via mcxn_usbdev_set_backend() and supply the
 * endpoint engine.  We play the usbredir "host" role (we export a device); the
 * remote peer is a stock `-device usb-redir` client.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "chardev/char-fe.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/mcxn_usbdev.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#include <usbredirparser.h>

#define USBDEV_VERSION "qemu mcxn-usbdev " QEMU_VERSION


/* ------------------------------------------------------------------------- *
 * chardev <-> usbredirparser bridge (mirrors hw/usb/redirect.c).
 * ------------------------------------------------------------------------- */

static int usbdev_read(void *priv, uint8_t *data, int count)
{
    MCXNUsbDevState *s = priv;

    if (s->read_buf_size < count) {
        count = s->read_buf_size;
    }
    if (count) {
        memcpy(data, s->read_buf, count);
        s->read_buf_size -= count;
        if (s->read_buf_size) {
            s->read_buf += count;
        } else {
            s->read_buf = NULL;
        }
    }
    return count;
}

static gboolean usbdev_write_unblocked(void *do_not_use, GIOCondition cond,
                                       void *opaque)
{
    MCXNUsbDevState *s = opaque;

    s->watch = 0;
    usbredirparser_do_write(s->parser);
    return FALSE;
}

static int usbdev_write(void *priv, uint8_t *data, int count)
{
    MCXNUsbDevState *s = priv;
    int r;

    if (!qemu_chr_fe_backend_open(&s->cs)) {
        return 0;
    }

    r = qemu_chr_fe_write(&s->cs, data, count);
    if (r < count) {
        if (!s->watch) {
            s->watch = qemu_chr_fe_add_watch(&s->cs, G_IO_OUT | G_IO_HUP,
                                             usbdev_write_unblocked, s);
        }
        if (r < 0) {
            r = 0;
        }
    }
    return r;
}

static void usbdev_log(void *priv, int level, const char *msg)
{
    qemu_log_mask(LOG_GUEST_ERROR, "mcxn-usbdev: %s\n", msg);
}

/* ------------------------------------------------------------------------- *
 * Endpoint-slot helpers + pending-request tracking.
 * ------------------------------------------------------------------------- */

static int usbdev_slot(uint8_t ep_addr)
{
    return (ep_addr & 0x0f) | ((ep_addr & 0x80) ? 0x10 : 0);
}

static MCXNUsbPending *usbdev_pending(MCXNUsbDevState *s, uint8_t ep_addr)
{
    return &s->pending[usbdev_slot(ep_addr)];
}

/* ------------------------------------------------------------------------- *
 * Device-side receive callbacks — forward host transfers to the controller
 * backend (guest firmware), reply over usbredir (sync or async).
 * ------------------------------------------------------------------------- */

static void usbdev_hello(void *priv, struct usb_redir_hello_header *h)
{
    MCXNUsbDevState *s = priv;

    s->connected = true;
    /* If firmware already enabled the controller before the peer connected,
     * (re)announce the device now that the handshake is complete. */
    if (s->attached && s->parser) {
        mcxn_usbdev_attach(s, s->speed);
    }
}

static void usbdev_reset(void *priv)
{
    MCXNUsbDevState *s = priv;

    memset(s->pending, 0, sizeof(s->pending));
    /* A USB bus reset reverts the device to the default (addr 0) state; let the
     * backend re-arm EP0 as firmware re-runs its reset ISR. */
    qemu_log_mask(LOG_GUEST_ERROR, "mcxn-usbdev: host bus reset\n");
}

/*
 * SET_CONFIGURATION / GET_CONFIGURATION / SET_INTERFACE / GET_INTERFACE arrive
 * as dedicated usbredir messages (not control_packets) from a real importer
 * (hw/usb/redirect.c sends usbredirparser_send_set_configuration etc.).  Drive
 * the no-data ones into the controller as a synthesized EP0 SETUP so guest
 * firmware transitions to Configured / selects the alt setting and arms its
 * endpoints, then reply with the matching status message.
 */
static void usbdev_std_setup(MCXNUsbDevState *s, uint8_t bmreq, uint8_t breq,
                             uint16_t val, uint16_t idx)
{
    uint8_t setup[8] = { bmreq, breq, val & 0xff, val >> 8,
                         idx & 0xff, idx >> 8, 0, 0 };
    if (s->be_ops) {
        s->be_ops->setup(s->be, setup);
    }
}

static void usbdev_set_configuration(void *priv, uint64_t id,
                                     struct usb_redir_set_configuration_header *h)
{
    MCXNUsbDevState *s = priv;
    MCXNUsbPending *p = usbdev_pending(s, 0x00);

    /* Defer configuration_status until firmware runs the SET_CONFIGURATION
     * status stage (complete_out): a real importer waits for this reply before
     * its next request, so deferring serializes the SETUPs and stops the next
     * one clobbering EP0 before firmware arms its endpoints.  Acking early is
     * the CDC ttyACM write-EIO / GET_LINE_CODING=0 bug — a multi-interface
     * gadget pipelines SET_CONFIGURATION -> SET_INTERFACE and the second SETUP
     * lands before firmware has processed the first. */
    s->cur_config = h->configuration;
    p->active = true;
    p->is_control = true;
    p->id = id;
    p->reply_kind = MCXN_USB_REPLY_CONFIG;
    p->arg0 = h->configuration;
    usbdev_std_setup(s, 0x00, 9, h->configuration, 0);   /* SET_CONFIGURATION */
}

static void usbdev_get_configuration(void *priv, uint64_t id)
{
    MCXNUsbDevState *s = priv;
    struct usb_redir_configuration_status_header st = { 0 };

    st.status = usb_redir_success;
    st.configuration = s->cur_config;
    usbredirparser_send_configuration_status(s->parser, id, &st);
    usbredirparser_do_write(s->parser);
}

static void usbdev_set_alt_setting(void *priv, uint64_t id,
                                   struct usb_redir_set_alt_setting_header *h)
{
    MCXNUsbDevState *s = priv;
    MCXNUsbPending *p = usbdev_pending(s, 0x00);

    /* Same deferral as SET_CONFIGURATION: ack only after firmware's status
     * stage, so the importer's next SETUP can't clobber EP0. */
    p->active = true;
    p->is_control = true;
    p->id = id;
    p->reply_kind = MCXN_USB_REPLY_ALT;
    p->arg0 = h->interface;
    p->arg1 = h->alt;
    usbdev_std_setup(s, 0x01, 11, h->alt, h->interface);  /* SET_INTERFACE */
}

static void usbdev_get_alt_setting(void *priv, uint64_t id,
                                   struct usb_redir_get_alt_setting_header *h)
{
    MCXNUsbDevState *s = priv;
    struct usb_redir_alt_setting_status_header st = { 0 };

    st.status = usb_redir_success;
    st.interface = h->interface;
    st.alt = 0;
    usbredirparser_send_alt_setting_status(s->parser, id, &st);
    usbredirparser_do_write(s->parser);
}

/*
 * Control transfer from the host.  Build the 8-byte SETUP, hand it to the
 * backend's EP0, then move the data stage:
 *   - IN  (requesttype bit7=1): pull up to @length bytes from EP0 IN.
 *   - OUT (requesttype bit7=0): push @data_len bytes to EP0 OUT, ack status.
 * A backend that must wait for firmware returns ASYNC; we stash the request and
 * answer from mcxn_usbdev_complete_in()/_out().
 */
static void usbdev_control_packet(void *priv, uint64_t id,
                                  struct usb_redir_control_packet_header *ch,
                                  uint8_t *data, int data_len)
{
    MCXNUsbDevState *s = priv;
    uint8_t setup[8];
    bool dev_to_host = ch->requesttype & 0x80;
    int rc;

    if (!s->be_ops) {
        ch->status = usb_redir_stall;
        usbredirparser_send_control_packet(s->parser, id, ch, NULL, 0);
        usbredirparser_free_packet_data(s->parser, data);
        return;
    }

    setup[0] = ch->requesttype;
    setup[1] = ch->request;
    setup[2] = ch->value & 0xff;
    setup[3] = ch->value >> 8;
    setup[4] = ch->index & 0xff;
    setup[5] = ch->index >> 8;
    setup[6] = ch->length & 0xff;
    setup[7] = ch->length >> 8;
    s->be_ops->setup(s->be, setup);

    if (dev_to_host) {
        g_autofree uint8_t *buf = g_malloc0(ch->length ? ch->length : 1);
        int out_len = 0;

        /* Register the pending request BEFORE invoking the backend: the engine
         * may complete synchronously (calling complete_in() inline), which must
         * find this request already active. */
        MCXNUsbPending *p = usbdev_pending(s, 0x80);
        p->active = true;
        p->is_control = true;
        p->id = id;
        p->length = ch->length;
        p->ep = 0x80;
        rc = s->be_ops->ep_in(s->be, 0, buf, ch->length, &out_len);
        if (rc != MCXN_USB_XFER_ASYNC) {
            p->active = false;
            ch->status = (rc == MCXN_USB_XFER_OK) ? usb_redir_success
                                                  : usb_redir_stall;
            usbredirparser_send_control_packet(s->parser, id, ch,
                                  rc == MCXN_USB_XFER_OK ? buf : NULL,
                                  rc == MCXN_USB_XFER_OK ? out_len : 0);
        }
    } else {
        /*
         * Host->device control.  Do NOT ack synchronously: a control transfer
         * only completes after its status stage, and acking early lets the host
         * pipeline the next SETUP — which would clobber the single in-flight
         * SETUP before firmware processes this one.  Mark it pending; the
         * backend completes it (via complete_out) once firmware runs the
         * zero-length status stage.
         */
        MCXNUsbPending *p = usbdev_pending(s, 0x00);
        p->active = true;
        p->is_control = true;
        p->id = id;
        p->ep = 0x00;
        p->reply_kind = MCXN_USB_REPLY_XFER;   /* slot 0 is shared with set_config/alt */
        if (data_len) {
            s->be_ops->ep_out(s->be, 0, data, data_len);
        }
    }
    usbredirparser_free_packet_data(s->parser, data);
}

/* Bulk transfer from the host: forward to the addressed endpoint. */
static void usbdev_bulk_packet(void *priv, uint64_t id,
                               struct usb_redir_bulk_packet_header *bh,
                               uint8_t *data, int data_len)
{
    MCXNUsbDevState *s = priv;
    bool dev_to_host = bh->endpoint & 0x80;
    int ep = bh->endpoint & 0x0f;
    uint16_t length = bh->length | ((uint32_t)bh->length_high << 16);
    int rc;

    if (!s->be_ops) {
        bh->status = usb_redir_stall;
        bh->length = 0;
        bh->length_high = 0;
        usbredirparser_send_bulk_packet(s->parser, id, bh, NULL, 0);
        usbredirparser_free_packet_data(s->parser, data);
        return;
    }

    if (dev_to_host) {
        g_autofree uint8_t *buf = g_malloc0(length ? length : 1);
        int out_len = 0;

        /* Mark pending before the call (engine may complete inline). */
        MCXNUsbPending *p = usbdev_pending(s, bh->endpoint);
        p->active = true;
        p->is_control = false;
        p->id = id;
        p->length = length;
        p->ep = bh->endpoint;
        rc = s->be_ops->ep_in(s->be, ep, buf, length, &out_len);
        if (rc != MCXN_USB_XFER_ASYNC) {
            p->active = false;
            bh->status = (rc == MCXN_USB_XFER_OK) ? usb_redir_success
                                                  : usb_redir_stall;
            bh->length = (rc == MCXN_USB_XFER_OK) ? (out_len & 0xffff) : 0;
            bh->length_high = (rc == MCXN_USB_XFER_OK) ? (out_len >> 16) : 0;
            usbredirparser_send_bulk_packet(s->parser, id, bh,
                                  rc == MCXN_USB_XFER_OK ? buf : NULL,
                                  rc == MCXN_USB_XFER_OK ? out_len : 0);
        }
    } else {
        MCXNUsbPending *p = usbdev_pending(s, bh->endpoint);
        p->active = true;
        p->is_control = false;
        p->id = id;
        p->ep = bh->endpoint;
        rc = s->be_ops->ep_out(s->be, ep, data, data_len);
        if (rc != MCXN_USB_XFER_ASYNC) {
            p->active = false;
            bh->status = (rc == MCXN_USB_XFER_OK) ? usb_redir_success
                                                  : usb_redir_stall;
            bh->length = 0;
            bh->length_high = 0;
            usbredirparser_send_bulk_packet(s->parser, id, bh, NULL, 0);
        }
    }
    usbredirparser_free_packet_data(s->parser, data);
}

/* ------------------------------------------------------------------------- *
 * Parser lifecycle.
 * ------------------------------------------------------------------------- */

static void usbdev_create_parser(MCXNUsbDevState *s)
{
    uint32_t caps[USB_REDIR_CAPS_SIZE] = { 0 };
    struct usbredirparser *p;

    p = usbredirparser_create();
    if (!p) {
        return;
    }
    p->priv = s;
    p->log_func = usbdev_log;
    p->read_func = usbdev_read;
    p->write_func = usbdev_write;
    p->hello_func = usbdev_hello;
    p->reset_func = usbdev_reset;
    p->set_configuration_func = usbdev_set_configuration;
    p->get_configuration_func = usbdev_get_configuration;
    p->set_alt_setting_func = usbdev_set_alt_setting;
    p->get_alt_setting_func = usbdev_get_alt_setting;
    p->control_packet_func = usbdev_control_packet;
    p->bulk_packet_func = usbdev_bulk_packet;

    /* We export a (virtual) device, i.e. we play the usb-host role; the remote
     * `-device usb-redir` is the client.  Advertise the standard caps. */
    usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);

    usbredirparser_init(p, USBDEV_VERSION, caps, USB_REDIR_CAPS_SIZE,
                        usbredirparser_fl_usb_host);
    s->parser = p;
    usbredirparser_do_write(p);
}

static void usbdev_destroy_parser(MCXNUsbDevState *s)
{
    if (s->watch) {
        g_source_remove(s->watch);
        s->watch = 0;
    }
    if (s->parser) {
        usbredirparser_destroy(s->parser);
        s->parser = NULL;
    }
    s->connected = false;
}

/* ------------------------------------------------------------------------- *
 * chardev event/read handlers.
 * ------------------------------------------------------------------------- */

static int usbdev_chr_can_read(void *opaque)
{
    MCXNUsbDevState *s = opaque;
    return s->parser ? 1024 * 1024 : 0;
}

static void usbdev_chr_read(void *opaque, const uint8_t *buf, int size)
{
    MCXNUsbDevState *s = opaque;

    if (!s->parser) {
        return;
    }
    s->read_buf = buf;
    s->read_buf_size = size;
    usbredirparser_do_read(s->parser);
    usbredirparser_do_write(s->parser);
}

static void usbdev_chr_event(void *opaque, QEMUChrEvent event)
{
    MCXNUsbDevState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        usbdev_destroy_parser(s);
        usbdev_create_parser(s);
        break;
    case CHR_EVENT_CLOSED:
        usbdev_destroy_parser(s);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- *
 * Public API.
 * ------------------------------------------------------------------------- */

void mcxn_usbdev_set_backend(MCXNUsbDevState *s,
                             const MCXNUsbBackendOps *ops, void *be)
{
    s->be_ops = ops;
    s->be = be;
}

void mcxn_usbdev_attach(MCXNUsbDevState *s, uint8_t speed)
{
    struct usb_redir_interface_info_header ii = { 0 };
    struct usb_redir_ep_info_header ei = { 0 };
    struct usb_redir_device_connect_header dc = { 0 };
    int i;

    s->attached = true;
    s->speed = speed;
    if (!s->parser || !s->connected) {
        return;     /* announced once the hello handshake completes */
    }

    /*
     * A real usb-redir *importer* (QEMU hw/usb/redirect.c usbredir_check_filter)
     * hard-requires interface_info to be set when device_connect is processed,
     * and uses ep_info to set up the endpoints — so both MUST be sent before
     * device_connect, even though the descriptors themselves still flow from
     * firmware via the forwarded control transfers.  The layout must match the
     * gadget firmware's config descriptor; gadget-profile selects it.  (A future
     * cleanup could derive this from the firmware's config descriptor.)
     */
    for (i = 0; i < 32; i++) {
        ei.type[i] = usb_redir_type_invalid;
    }
    /* EP0 is always control/64; bulk max-packet is speed-coherent (HS=512). */
    uint16_t bulk_mps = (speed == usb_redir_speed_high) ? 512 : 64;
    ei.type[0]  = usb_redir_type_control; ei.max_packet_size[0]  = 64;   /* EP0 OUT */
    ei.type[16] = usb_redir_type_control; ei.max_packet_size[16] = 64;   /* EP0 IN  */

    if (s->gadget_profile && !strcmp(s->gadget_profile, "cdc")) {
        /* CDC-ACM: interface 0 Communications/ACM + EP2-IN interrupt notify;
         * interface 1 Data + EP1 bulk in/out.  Binds Linux cdc_acm. */
        ii.interface_count = 2;
        ii.interface[0] = 0;
        ii.interface_class[0] = 0x02;      /* Communications */
        ii.interface_subclass[0] = 0x02;   /* Abstract Control Model */
        ii.interface_protocol[0] = 0x01;   /* AT commands (V.25ter) */
        ii.interface[1] = 1;
        ii.interface_class[1] = 0x0A;      /* CDC Data */
        ii.interface_subclass[1] = 0;
        ii.interface_protocol[1] = 0;
        ei.type[18] = usb_redir_type_interrupt;  /* EP2 IN (0x82) notify */
        ei.max_packet_size[18] = 16; ei.interval[18] = 9; ei.interface[18] = 0;
        ei.type[1]  = usb_redir_type_bulk;       /* EP1 OUT data */
        ei.max_packet_size[1] = bulk_mps; ei.interface[1] = 1;
        ei.type[17] = usb_redir_type_bulk;       /* EP1 IN  data */
        ei.max_packet_size[17] = bulk_mps; ei.interface[17] = 1;
    } else {
        /* Vendor: one vendor-class interface, EP0 control + EP1 bulk in/out. */
        ii.interface_count = 1;
        ii.interface[0] = 0;
        ii.interface_class[0] = 0xFF;      /* vendor-specific */
        ii.interface_subclass[0] = 0;
        ii.interface_protocol[0] = 0;
        ei.type[1]  = usb_redir_type_bulk; ei.max_packet_size[1]  = bulk_mps;
        ei.type[17] = usb_redir_type_bulk; ei.max_packet_size[17] = bulk_mps;
        ei.interface[1] = ei.interface[17] = 0;
    }
    usbredirparser_send_interface_info(s->parser, &ii);
    usbredirparser_send_ep_info(s->parser, &ei);

    /* Descriptors are sourced from firmware via forwarded control transfers, so
     * the connect header carries only the speed; the host learns class/ids from
     * the real GET_DESCRIPTOR responses. */
    dc.speed = speed;
    usbredirparser_send_device_connect(s->parser, &dc);
    usbredirparser_do_write(s->parser);
}

void mcxn_usbdev_detach(MCXNUsbDevState *s)
{
    s->attached = false;
    memset(s->pending, 0, sizeof(s->pending));
    if (s->parser && s->connected) {
        usbredirparser_send_device_disconnect(s->parser);
        usbredirparser_do_write(s->parser);
    }
}

/* Backend retired a primed IN descriptor that answers a pending host IN. */
void mcxn_usbdev_complete_in(MCXNUsbDevState *s, int ep,
                             const uint8_t *buf, int len)
{
    uint8_t ep_addr = (ep & 0x0f) | 0x80;
    MCXNUsbPending *p = usbdev_pending(s, ep_addr);

    if (!p->active || !s->parser) {
        return;
    }
    if (len > p->length) {
        len = p->length;
    }

    if (p->is_control) {
        struct usb_redir_control_packet_header ch = { 0 };
        ch.endpoint = ep_addr;
        ch.status = usb_redir_success;
        ch.length = len;
        usbredirparser_send_control_packet(s->parser, p->id, &ch,
                                           (uint8_t *)buf, len);
    } else {
        struct usb_redir_bulk_packet_header bh = { 0 };
        bh.endpoint = ep_addr;
        bh.status = usb_redir_success;
        bh.length = len & 0xffff;
        bh.length_high = len >> 16;
        usbredirparser_send_bulk_packet(s->parser, p->id, &bh,
                                        (uint8_t *)buf, len);
    }
    p->active = false;
    usbredirparser_do_write(s->parser);
}

/* Backend retired a primed OUT descriptor that answers a pending host OUT. */
void mcxn_usbdev_complete_out(MCXNUsbDevState *s, int ep, int status, int len)
{
    uint8_t ep_addr = ep & 0x0f;
    MCXNUsbPending *p = usbdev_pending(s, ep_addr);
    uint8_t st = (status == MCXN_USB_XFER_OK) ? usb_redir_success
                                              : usb_redir_stall;

    if (!p->active || !s->parser) {
        return;
    }
    if (p->reply_kind == MCXN_USB_REPLY_CONFIG) {
        /* Deferred SET_CONFIGURATION ack — firmware just ran the status stage. */
        struct usb_redir_configuration_status_header cs = { 0 };
        cs.status = st;
        cs.configuration = p->arg0;
        usbredirparser_send_configuration_status(s->parser, p->id, &cs);
    } else if (p->reply_kind == MCXN_USB_REPLY_ALT) {
        /* Deferred SET_INTERFACE ack. */
        struct usb_redir_alt_setting_status_header as = { 0 };
        as.status = st;
        as.interface = p->arg0;
        as.alt = p->arg1;
        usbredirparser_send_alt_setting_status(s->parser, p->id, &as);
    } else if (p->is_control) {
        struct usb_redir_control_packet_header ch = { 0 };
        ch.endpoint = ep_addr;
        ch.status = st;
        ch.length = len & 0xffff;
        usbredirparser_send_control_packet(s->parser, p->id, &ch, NULL, 0);
    } else {
        struct usb_redir_bulk_packet_header bh = { 0 };
        bh.endpoint = ep_addr;
        bh.status = st;
        /* Report the actual transferred byte count: a real OUT-endpoint driver
         * (e.g. cdc_acm's tty write) reads this and treats 0 as a short write. */
        bh.length = len & 0xffff;
        bh.length_high = (len >> 16) & 0xffff;
        usbredirparser_send_bulk_packet(s->parser, p->id, &bh, NULL, 0);
    }
    p->active = false;
    p->reply_kind = MCXN_USB_REPLY_XFER;
    usbredirparser_do_write(s->parser);
}

/* ------------------------------------------------------------------------- *
 * QOM.
 * ------------------------------------------------------------------------- */

static void mcxn_usbdev_realize(DeviceState *dev, Error **errp)
{
    MCXNUsbDevState *s = MCXN_USBDEV(dev);

    if (qemu_chr_fe_backend_connected(&s->cs)) {
        qemu_chr_fe_set_handlers(&s->cs, usbdev_chr_can_read, usbdev_chr_read,
                                 usbdev_chr_event, NULL, s, NULL, true);
    }
}

static void mcxn_usbdev_unrealize(DeviceState *dev)
{
    MCXNUsbDevState *s = MCXN_USBDEV(dev);

    usbdev_destroy_parser(s);
}

static const Property mcxn_usbdev_props[] = {
    DEFINE_PROP_CHR("chardev", MCXNUsbDevState, cs),
    DEFINE_PROP_STRING("gadget-profile", MCXNUsbDevState, gadget_profile),
};

static void mcxn_usbdev_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_usbdev_realize;
    dc->unrealize = mcxn_usbdev_unrealize;
    device_class_set_props(dc, mcxn_usbdev_props);
    /* No user-creatable: the SoC instantiates and links it to a backend. */
    dc->user_creatable = false;
}

static const TypeInfo mcxn_usbdev_types[] = {
    {
        .name          = TYPE_MCXN_USBDEV,
        .parent        = TYPE_DEVICE,
        .instance_size = sizeof(MCXNUsbDevState),
        .class_init    = mcxn_usbdev_class_init,
    },
};

DEFINE_TYPES(mcxn_usbdev_types)
