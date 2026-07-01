/*
 * NXP MCX N USBFS0 — USB Full-Speed (KHCI) controller, device mode.
 *
 * Register-accurate KHCI model plus a device-mode endpoint engine.  Guest
 * firmware arms Buffer Descriptors (BDs) in a RAM-resident BDT (ping-pong banks
 * per endpoint/direction); a remote USB host drives transactions through the
 * shared usbredir core (hw/usb/mcxn_usbdev.c).  We present SETUP/IN/OUT tokens
 * to firmware (STAT + ISTAT.TOKDNE), read/retire the BDs it armed, and relay the
 * descriptor-driven data back to the host.  Host token retries are modelled by a
 * 1 ms SOF tick (and re-checked when firmware acks TOKDNE), so the data stage of
 * a control transfer completes once the firmware ISR has armed the answering BD.
 *
 * Offsets/bits from the MCXN947 CMSIS header (USB_Type / PERI_USB.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "exec/cpu-common.h"
#include "hw/misc/mcxn_usbfs.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#include <usbredirproto.h>

/* Register offsets (8-bit registers on 32-bit stride; CMSIS USB_Type). */
#define R_PERID     0x00    /* RO */
#define R_IDCOMP    0x04    /* RO */
#define R_REV       0x08    /* RO */
#define R_ADDINFO   0x0C    /* RO */
#define R_OTGISTAT  0x10    /* W1C */
#define R_OTGICR    0x14
#define R_OTGSTAT   0x18    /* RO */
#define R_OTGCTL    0x1C
#define R_ISTAT     0x80    /* W1C */
#define R_INTEN     0x84
#define R_ERRSTAT   0x88    /* W1C */
#define R_ERREN     0x8C
#define R_STAT      0x90    /* RO */
#define R_CTL       0x94
#define R_ADDR      0x98
#define R_BDTPAGE1  0x9C
#define R_FRMNUML   0xA0
#define R_FRMNUMH   0xA4
#define R_TOKEN     0xA8
#define R_BDTPAGE2  0xB0
#define R_BDTPAGE3  0xB4
#define R_ENDPT0    0xC0    /* ENDPT[0..15] @ 0xC0, step 4 */
#define R_USBCTRL   0x100
#define R_USBTRC0   0x10C

/* CTL bits. */
#define CTL_USBENSOFEN  (1u << 0)
#define CTL_ODDRST      (1u << 1)
#define CTL_HOSTMODEEN  (1u << 3)
#define USBTRC0_USBRESET (1u << 7)

/* ISTAT / INTEN bits. */
#define ISTAT_USBRST    (1u << 0)
#define ISTAT_ERROR     (1u << 1)
#define ISTAT_SOFTOK    (1u << 2)
#define ISTAT_TOKDNE    (1u << 3)
#define ISTAT_SLEEP     (1u << 4)
#define ISTAT_STALL     (1u << 7)

/* STAT fields. */
#define STAT_ODD_SHIFT  2
#define STAT_TX         (1u << 3)
#define STAT_ENDP_SHIFT 4

/* ENDPT bits. */
#define ENDPT_EPHSHK    (1u << 0)
#define ENDPT_EPSTALL   (1u << 1)
#define ENDPT_EPTXEN    (1u << 2)
#define ENDPT_EPRXEN    (1u << 3)

/* BD control word (SW view) bits, and completion fields. */
#define BD_STALL    (1u << 2)
#define BD_DTS      (1u << 3)
#define BD_NINC     (1u << 4)
#define BD_KEEP     (1u << 5)
#define BD_DATA1    (1u << 6)
#define BD_OWN      (1u << 7)
#define BD_BC_SHIFT     16
#define BD_BC_MASK      0x3FF       /* 10-bit byte count */
#define BD_TOKPID_SHIFT 2
#define BD_TOKPID_MASK  0xF

/* Token PIDs (as reported in the retired BD's tok_pid field). */
#define PID_OUT     0x1
#define PID_IN      0x9
#define PID_SETUP   0xD

#define PERID_VALUE     0x04
#define IDCOMP_VALUE    0xFB
#define REV_VALUE       0x33

#define SOF_PERIOD_NS   (1 * 1000 * 1000)   /* 1 ms */

static void usbfs_update_irq(MCXNUSBFSState *s)
{
    bool active =
        ((s->regs[R_ISTAT / 4]    & s->regs[R_INTEN / 4])  & 0xFF) ||
        ((s->regs[R_ERRSTAT / 4]  & s->regs[R_ERREN / 4])  & 0xFF) ||
        ((s->regs[R_OTGISTAT / 4] & s->regs[R_OTGICR / 4]) & 0xFF);

    qemu_set_irq(s->irq, active);
}

/* ------------------------------------------------------------------------- *
 * BDT access.  Base = {BDTPAGE3,BDTPAGE2,BDTPAGE1,0} (512-byte aligned).
 * BD index = (ep*4) + (tx ? 2 : 0) + odd; each BD is 8 bytes (ctrl, buf).
 * ------------------------------------------------------------------------- */

static uint32_t bdt_base(MCXNUSBFSState *s)
{
    return ((s->regs[R_BDTPAGE1 / 4] & 0xFF) << 8)  |
           ((s->regs[R_BDTPAGE2 / 4] & 0xFF) << 16) |
           ((s->regs[R_BDTPAGE3 / 4] & 0xFF) << 24);
}

static uint32_t bd_off(int ep, bool tx, int odd)
{
    return (((ep * 4) + (tx ? 2 : 0) + (odd & 1)) * 8);
}

static uint32_t bd_ld(uint32_t addr)
{
    uint32_t w = 0;
    cpu_physical_memory_read(addr, &w, 4);
    return le32_to_cpu(w);
}

static void bd_st(uint32_t addr, uint32_t w)
{
    uint32_t t = cpu_to_le32(w);
    cpu_physical_memory_write(addr, &t, 4);
}

/* Raise TOKDNE for a just-retired BD and stash which one in STAT. */
static void usbfs_tokdne(MCXNUSBFSState *s, int ep, bool tx, int odd)
{
    s->regs[R_STAT / 4] = (ep << STAT_ENDP_SHIFT) |
                          (tx ? STAT_TX : 0) |
                          ((odd & 1) << STAT_ODD_SHIFT);
    s->regs[R_ISTAT / 4] |= ISTAT_TOKDNE;
    s->tokdne_busy = true;
    usbfs_update_irq(s);
}

/* ------------------------------------------------------------------------- *
 * Endpoint servicing — runs when a BD might be ready (SOF tick / TOKDNE ack).
 * Only one token is outstanding to firmware at a time (tokdne_busy gate), as
 * with the real STAT/TOKDNE handshake.
 * ------------------------------------------------------------------------- */

static bool usbfs_deliver_setup(MCXNUSBFSState *s)
{
    int odd = s->odd_rx[0] & 1;
    uint32_t base = bdt_base(s);
    uint32_t ba = base + bd_off(0, false, odd);
    uint32_t ctrl = bd_ld(ba);

    if (!(ctrl & BD_OWN)) {
        return false;                       /* firmware hasn't armed EP0 RX */
    }
    cpu_physical_memory_write(bd_ld(ba + 4), s->setup_pkt, 8);
    bd_st(ba, (8u << BD_BC_SHIFT) | (PID_SETUP << BD_TOKPID_SHIFT));
    s->odd_rx[0] ^= 1;
    s->setup_pending = false;
    usbfs_tokdne(s, 0, false, odd);
    return true;
}

static bool usbfs_service_in(MCXNUSBFSState *s, int ep)
{
    MCXNUSBFSXfer *x = &s->ep[ep];
    int odd = s->odd_tx[ep] & 1;
    uint32_t base = bdt_base(s);
    uint32_t ba = base + bd_off(ep, true, odd);
    uint32_t ctrl = bd_ld(ba);
    int bc;

    if (!(ctrl & BD_OWN)) {
        return false;                       /* firmware hasn't armed IN data */
    }
    bc = (ctrl >> BD_BC_SHIFT) & BD_BC_MASK;
    if (bc > MCXN_USBFS_MPS) {
        bc = MCXN_USBFS_MPS;
    }
    if (x->in_acc + bc > MCXN_USBFS_XFERMAX) {
        bc = MCXN_USBFS_XFERMAX - x->in_acc;
    }
    cpu_physical_memory_read(bd_ld(ba + 4), x->in_buf + x->in_acc, bc);
    x->in_acc += bc;
    bd_st(ba, ((uint32_t)bc << BD_BC_SHIFT) | (PID_IN << BD_TOKPID_SHIFT));
    s->odd_tx[ep] ^= 1;
    usbfs_tokdne(s, ep, true, odd);

    /* Transfer complete on a short packet or once the host's ask is met. */
    if (bc < MCXN_USBFS_MPS || x->in_acc >= x->in_len ||
        x->in_acc >= MCXN_USBFS_XFERMAX) {
        x->in_pending = false;
        mcxn_usbdev_complete_in(s->usbdev, ep, x->in_buf, x->in_acc);
    }
    return true;
}

static bool usbfs_service_out(MCXNUSBFSState *s, int ep)
{
    MCXNUSBFSXfer *x = &s->ep[ep];
    int odd = s->odd_rx[ep] & 1;
    uint32_t base = bdt_base(s);
    uint32_t ba = base + bd_off(ep, false, odd);
    uint32_t ctrl = bd_ld(ba);
    int chunk = x->out_len - x->out_off;

    if (!(ctrl & BD_OWN)) {
        return false;                       /* firmware hasn't armed RX buf */
    }
    if (chunk > MCXN_USBFS_MPS) {
        chunk = MCXN_USBFS_MPS;
    }
    cpu_physical_memory_write(bd_ld(ba + 4), x->out_buf + x->out_off, chunk);
    x->out_off += chunk;
    bd_st(ba, ((uint32_t)chunk << BD_BC_SHIFT) | (PID_OUT << BD_TOKPID_SHIFT));
    s->odd_rx[ep] ^= 1;
    usbfs_tokdne(s, ep, false, odd);

    if (x->out_off >= x->out_len || chunk < MCXN_USBFS_MPS) {
        x->out_pending = false;
        mcxn_usbdev_complete_out(s->usbdev, ep, MCXN_USB_XFER_OK, x->out_len);
    }
    return true;
}

/*
 * Consume the firmware's zero-length status-stage IN BD for a no-data (or OUT)
 * control transfer.  usbredir collapses the 3-stage control handshake into one
 * message and the core acks it directly, but real firmware still arms a status
 * BD per stage — draining it here keeps the EP0 TX ping-pong bank in sync with
 * firmware so the next IN reads the right bank.
 */
static bool usbfs_consume_status_in(MCXNUSBFSState *s)
{
    int odd = s->odd_tx[0] & 1;
    uint32_t ba = bdt_base(s) + bd_off(0, true, odd);
    uint32_t ctrl = bd_ld(ba);

    if (!(ctrl & BD_OWN)) {
        return false;                       /* firmware hasn't armed it yet */
    }
    bd_st(ba, (PID_IN << BD_TOKPID_SHIFT));  /* retire, zero byte count */
    s->odd_tx[0] ^= 1;
    s->ep0_status_in = false;
    usbfs_tokdne(s, 0, true, odd);
    /* The host->device control transfer is now complete (firmware saw the SETUP
     * and ran the status stage): ack the host.  No-op if a data stage already
     * completed it. */
    mcxn_usbdev_complete_out(s->usbdev, 0, MCXN_USB_XFER_OK, 0);
    return true;
}

/* Try to make progress on one outstanding transaction. */
static void usbfs_service(MCXNUSBFSState *s)
{
    int ep;

    if (!s->enabled || s->tokdne_busy) {
        return;                             /* wait for firmware to ack TOKDNE */
    }
    if (s->ep0_status_in && usbfs_consume_status_in(s)) {
        return;
    }
    if (s->setup_pending && usbfs_deliver_setup(s)) {
        return;
    }
    for (ep = 0; ep < MCXN_USBFS_NEP; ep++) {
        if (s->ep[ep].in_pending && usbfs_service_in(s, ep)) {
            return;
        }
        if (s->ep[ep].out_pending && usbfs_service_out(s, ep)) {
            return;
        }
    }
}

static void usbfs_sof(void *opaque)
{
    MCXNUSBFSState *s = opaque;

    if (!s->enabled) {
        return;
    }
    s->regs[R_FRMNUML / 4] = (s->regs[R_FRMNUML / 4] + 1) & 0xFF;
    s->regs[R_ISTAT / 4] |= ISTAT_SOFTOK;
    usbfs_update_irq(s);
    usbfs_service(s);
    timer_mod(s->sof, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SOF_PERIOD_NS);
}

/* ------------------------------------------------------------------------- *
 * usbredir backend ops — host transactions arrive here from the core.
 * ------------------------------------------------------------------------- */

static void usbfs_be_setup(void *be, const uint8_t setup[8])
{
    MCXNUSBFSState *s = be;

    memcpy(s->setup_pkt, setup, 8);
    s->setup_pending = true;
    /* Host->device control (SETUP bmRequestType bit7=0) has a zero-length IN
     * status stage the firmware will arm; remember to drain it.  Only SET the
     * flag (never clear it for an IN transfer) so a still-pending status from a
     * prior OUT transfer survives — the tokdne_busy gate drains it before this
     * SETUP is delivered. */
    if (!(setup[0] & 0x80)) {
        s->ep0_status_in = true;
    }
    /* Reset EP0 transfer accumulators for the new control transaction. */
    s->ep[0].in_pending = false;
    s->ep[0].in_acc = 0;
    s->ep[0].out_pending = false;
    s->ep[0].out_off = 0;
    usbfs_service(s);
}

static int usbfs_be_ep_in(void *be, int ep, uint8_t *buf, int len, int *out_len)
{
    MCXNUSBFSState *s = be;
    MCXNUSBFSXfer *x = &s->ep[ep & 0xf];

    if ((s->regs[(R_ENDPT0 + (ep & 0xf) * 4) / 4] & ENDPT_EPSTALL)) {
        return MCXN_USB_XFER_STALL;
    }
    x->in_pending = true;
    x->in_len = len;
    x->in_acc = 0;
    *out_len = 0;
    usbfs_service(s);
    return MCXN_USB_XFER_ASYNC;     /* answered later via complete_in() */
}

static int usbfs_be_ep_out(void *be, int ep, const uint8_t *buf, int len)
{
    MCXNUSBFSState *s = be;
    MCXNUSBFSXfer *x = &s->ep[ep & 0xf];

    if ((s->regs[(R_ENDPT0 + (ep & 0xf) * 4) / 4] & ENDPT_EPSTALL)) {
        return MCXN_USB_XFER_STALL;
    }
    if (len > MCXN_USBFS_XFERMAX) {
        len = MCXN_USBFS_XFERMAX;
    }
    if (len) {
        memcpy(x->out_buf, buf, len);
    }
    x->out_len = len;
    x->out_off = 0;
    x->out_pending = true;
    usbfs_service(s);
    return MCXN_USB_XFER_ASYNC;     /* answered later via complete_out() */
}

static void usbfs_be_set_address(void *be, uint8_t addr) { /* firmware writes ADDR */ }
static void usbfs_be_set_config(void *be, uint8_t cfg)   { /* firmware handles */ }

static const MCXNUsbBackendOps usbfs_be_ops = {
    .setup       = usbfs_be_setup,
    .ep_in       = usbfs_be_ep_in,
    .ep_out      = usbfs_be_ep_out,
    .set_address = usbfs_be_set_address,
    .set_config  = usbfs_be_set_config,
};

/* ------------------------------------------------------------------------- *
 * MMIO.
 * ------------------------------------------------------------------------- */

static uint64_t mcxn_usbfs_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBFSState *s = MCXN_USBFS(opaque);

    switch (off) {
    case R_PERID:   return PERID_VALUE;
    case R_IDCOMP:  return IDCOMP_VALUE;
    case R_REV:     return REV_VALUE;
    case R_USBTRC0:
        return s->regs[R_USBTRC0 / 4] & ~USBTRC0_USBRESET;
    default:
        return (off < MCXN_USBFS_SIZE) ? s->regs[off >> 2] : 0;
    }
}

static void mcxn_usbfs_write(void *opaque, hwaddr off, uint64_t value,
                             unsigned size)
{
    MCXNUSBFSState *s = MCXN_USBFS(opaque);
    uint32_t v = value;

    if (off >= MCXN_USBFS_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_PERID:
    case R_IDCOMP:
    case R_REV:
    case R_ADDINFO:
    case R_OTGSTAT:
    case R_STAT:
        return;                             /* read-only */
    case R_OTGISTAT:
    case R_ERRSTAT:
        s->regs[off >> 2] &= ~(v & 0xFF);   /* W1C */
        usbfs_update_irq(s);
        return;
    case R_ISTAT:
        s->regs[off >> 2] &= ~(v & 0xFF);   /* W1C */
        if (v & ISTAT_TOKDNE) {
            /* Firmware consumed the token: STAT advances, service the next. */
            s->tokdne_busy = false;
            usbfs_update_irq(s);
            usbfs_service(s);
            return;
        }
        usbfs_update_irq(s);
        return;
    case R_CTL:
        s->regs[off >> 2] = v;
        if (v & CTL_ODDRST) {
            memset(s->odd_rx, 0, sizeof(s->odd_rx));
            memset(s->odd_tx, 0, sizeof(s->odd_tx));
        }
        if ((v & CTL_USBENSOFEN) && !(v & CTL_HOSTMODEEN) && !s->enabled) {
            /* Device mode enabled + pull-up: a device appears on the bus. */
            s->enabled = true;
            timer_mod(s->sof,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SOF_PERIOD_NS);
            mcxn_usbdev_attach(s->usbdev, usb_redir_speed_full);
        } else if (!(v & CTL_USBENSOFEN) && s->enabled) {
            s->enabled = false;
            timer_del(s->sof);
            mcxn_usbdev_detach(s->usbdev);
        }
        return;
    case R_USBTRC0:
        s->regs[off >> 2] = v & ~USBTRC0_USBRESET;
        return;
    case R_INTEN:
    case R_ERREN:
    case R_OTGICR:
        s->regs[off >> 2] = v;
        usbfs_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_usbfs_ops = {
    .read = mcxn_usbfs_read,
    .write = mcxn_usbfs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_usbfs_reset(DeviceState *dev)
{
    MCXNUSBFSState *s = MCXN_USBFS(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->odd_rx, 0, sizeof(s->odd_rx));
    memset(s->odd_tx, 0, sizeof(s->odd_tx));
    memset(s->ep, 0, sizeof(s->ep));
    s->enabled = false;
    s->tokdne_busy = false;
    s->setup_pending = false;
    timer_del(s->sof);
}

static void mcxn_usbfs_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBFSState *s = MCXN_USBFS(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_usbfs_ops, s,
                          TYPE_MCXN_USBFS, MCXN_USBFS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->sof = timer_new_ns(QEMU_CLOCK_VIRTUAL, usbfs_sof, s);

    if (s->usbdev) {
        mcxn_usbdev_set_backend(s->usbdev, &usbfs_be_ops, s);
    }
}

static const VMStateDescription vmstate_mcxn_usbfs = {
    .name = TYPE_MCXN_USBFS,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBFSState, MCXN_USBFS_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mcxn_usbfs_props[] = {
    DEFINE_PROP_LINK("usbdev", MCXNUSBFSState, usbdev, TYPE_MCXN_USBDEV,
                     MCXNUsbDevState *),
};

static void mcxn_usbfs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_usbfs_realize;
    device_class_set_legacy_reset(dc, mcxn_usbfs_reset);
    device_class_set_props(dc, mcxn_usbfs_props);
    dc->vmsd = &vmstate_mcxn_usbfs;
}

static const TypeInfo mcxn_usbfs_types[] = {
    {
        .name          = TYPE_MCXN_USBFS,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUSBFSState),
        .class_init    = mcxn_usbfs_class_init,
    },
};

DEFINE_TYPES(mcxn_usbfs_types)
