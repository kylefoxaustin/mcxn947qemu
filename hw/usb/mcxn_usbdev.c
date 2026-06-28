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
 * Device-side receive callbacks — STUBS for now (enumeration lands next).
 * ------------------------------------------------------------------------- */

static void usbdev_hello(void *priv, struct usb_redir_hello_header *h)
{
    MCXNUsbDevState *s = priv;

    s->connected = true;
    qemu_log_mask(LOG_UNIMP, "mcxn-usbdev: hello from peer (enumeration TODO)\n");
}

static void usbdev_reset(void *priv)
{
    qemu_log_mask(LOG_UNIMP, "mcxn-usbdev: bus reset (TODO)\n");
}

static void usbdev_control_packet(void *priv, uint64_t id,
                                  struct usb_redir_control_packet_header *ch,
                                  uint8_t *data, int data_len)
{
    qemu_log_mask(LOG_UNIMP, "mcxn-usbdev: control packet (TODO)\n");
    usbredirparser_free_packet_data(((MCXNUsbDevState *)priv)->parser, data);
}

static void usbdev_bulk_packet(void *priv, uint64_t id,
                               struct usb_redir_bulk_packet_header *bh,
                               uint8_t *data, int data_len)
{
    qemu_log_mask(LOG_UNIMP, "mcxn-usbdev: bulk packet (TODO)\n");
    usbredirparser_free_packet_data(((MCXNUsbDevState *)priv)->parser, data);
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
    p->control_packet_func = usbdev_control_packet;
    p->bulk_packet_func = usbdev_bulk_packet;

    /* We export a (virtual) device, i.e. we play the usb-host role; the remote
     * `-device usb-redir` is the client.  Advertise the standard caps. */
    usbredirparser_caps_set_cap(caps, usb_redir_cap_connect_device_version);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_ep_info_max_packet_size);
    usbredirparser_caps_set_cap(caps, usb_redir_cap_64bits_ids);

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

void mcxn_usbdev_complete_in(MCXNUsbDevState *s, int ep,
                             const uint8_t *buf, int len)
{
    /* Async IN completion -> usbredir reply.  Filled in with enumeration. */
    qemu_log_mask(LOG_UNIMP, "mcxn-usbdev: complete_in ep%d (TODO)\n", ep);
}

void mcxn_usbdev_complete_out(MCXNUsbDevState *s, int ep, int status)
{
    qemu_log_mask(LOG_UNIMP, "mcxn-usbdev: complete_out ep%d (TODO)\n", ep);
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
