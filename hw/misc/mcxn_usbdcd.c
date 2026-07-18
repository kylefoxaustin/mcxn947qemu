/*
 * NXP MCX N USBDCD0 — USB Device Charger Detection (BC1.2 sequence).
 *
 * Firmware runs the standard USB Battery-Charging-1.2 detection: write CONTROL.START,
 * then walk STATUS[SEQ_STAT]/[SEQ_RES] with one interrupt (CONTROL.IF, gated by IE) per
 * phase, acknowledging each with CONTROL.IACK.  The block sequences the phases exactly as
 * the silicon does; the ONE thing it cannot know -- what is physically on the port -- is
 * operator-driven via the `charger` QOM property, never fabricated:
 *
 *   contact detect  -> SEQ_STAT=01
 *   primary detect  -> SEQ_STAT=10, SEQ_RES=01 (SDP, done) or 10 (a charging port)
 *   secondary detect-> SEQ_STAT=11, SEQ_RES=10 (CDP) or 11 (DCP)
 *   nothing attached-> STATUS[TO] (data-pin contact timed out), no classification
 *
 * ⚠ THE ATTACHED PORT IS AN OPERATOR INPUT, NOT A MEASUREMENT.  With no charger set, the
 *   honest answer is a contact-detect TIMEOUT (nothing is plugged in) -- not a fabricated
 *   "SDP".  Set `-global mcxn-usbdcd.charger=1|2|3` (SDP/CDP/DCP), the way the analog
 *   inputs are operator-driven, to have the sequence classify a port.
 *
 * Offsets/bits from the MCXN947 CMSIS header (USBDCD_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_usbdcd.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS USBDCD_Type). */
#define R_CONTROL          0x00
#define R_CLOCK            0x04
#define R_STATUS           0x08    /* RO */
#define R_SIGNAL_OVERRIDE  0x0C
#define R_TIMER0           0x10
#define R_TIMER1           0x14
#define R_TIMER2           0x18

/* CONTROL bits. */
#define CONTROL_IACK    (1u << 0)
#define CONTROL_IF      (1u << 8)
#define CONTROL_IE      (1u << 16)
#define CONTROL_BC12    (1u << 17)
#define CONTROL_START   (1u << 24)
#define CONTROL_SR      (1u << 25)

/* STATUS fields. */
#define STATUS_SEQ_RES(v)   (((v) & 3u) << 16)
#define STATUS_SEQ_STAT(v)  (((v) & 3u) << 18)
#define STATUS_ERR          (1u << 20)
#define STATUS_TO           (1u << 21)
#define STATUS_ACTIVE       (1u << 22)

/* SEQ_RES encodings. */
#define RES_NONE  0
#define RES_SDP   1
#define RES_CHG   2    /* a charging port (CDP when SEQ_STAT=11, else undetermined) */
#define RES_DCP   3
/* SEQ_STAT phases. */
#define STAT_NONE     0
#define STAT_CONTACT  1
#define STAT_PRIMARY  2
#define STAT_TYPE     3

static void dcd_update_irq(MCXNUSBDCDState *s)
{
    bool level = (s->regs[R_CONTROL / 4] & CONTROL_IF) &&
                 (s->regs[R_CONTROL / 4] & CONTROL_IE);

    qemu_set_irq(s->irq, level);
}

/* Latch a phase result and raise the per-phase interrupt flag. */
static void dcd_phase_done(MCXNUSBDCDState *s, uint32_t stat, uint32_t res,
                           uint32_t extra)
{
    s->regs[R_STATUS / 4] = STATUS_SEQ_STAT(stat) | STATUS_SEQ_RES(res) | extra;
    s->regs[R_CONTROL / 4] |= CONTROL_IF;
    dcd_update_irq(s);
}

static void dcd_start(MCXNUSBDCDState *s)
{
    if (s->charger == MCXN_DCD_NONE) {
        /* No data-pin contact: the sequence times out.  Honest "nothing plugged
         * in" -- NOT a fabricated classification. */
        s->phase = 0;
        dcd_phase_done(s, STAT_NONE, RES_NONE, STATUS_TO);
        return;
    }
    s->phase = STAT_CONTACT;
    dcd_phase_done(s, STAT_CONTACT, RES_NONE, STATUS_ACTIVE);
}

/* On IACK, the silicon proceeds to the next detection phase. */
static void dcd_advance(MCXNUSBDCDState *s)
{
    switch (s->phase) {
    case STAT_CONTACT:                       /* -> primary: SDP vs charging port */
        if (s->charger == MCXN_DCD_SDP) {
            s->phase = 0;                    /* SDP fully classified, sequence done */
            dcd_phase_done(s, STAT_PRIMARY, RES_SDP, 0);
        } else {
            s->phase = STAT_PRIMARY;         /* a charging port; type still unknown */
            dcd_phase_done(s, STAT_PRIMARY, RES_CHG, STATUS_ACTIVE);
        }
        break;
    case STAT_PRIMARY:                       /* -> secondary: CDP vs DCP */
        s->phase = 0;
        dcd_phase_done(s, STAT_TYPE,
                       s->charger == MCXN_DCD_CDP ? RES_CHG : RES_DCP, 0);
        break;
    default:
        break;                               /* sequence already complete */
    }
}

static uint64_t mcxn_usbdcd_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBDCDState *s = MCXN_USBDCD(opaque);

    switch (off) {
    case R_CONTROL:
        /* START/SR/IACK are write-only / self-clearing; IF, IE, BC12 read back. */
        return s->regs[R_CONTROL / 4] & (CONTROL_IF | CONTROL_IE | CONTROL_BC12);
    case R_STATUS:
        return s->regs[R_STATUS / 4];
    default:
        return (off < MCXN_USBDCD_SIZE) ? s->regs[off >> 2] : 0;
    }
}

static void mcxn_usbdcd_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size)
{
    MCXNUSBDCDState *s = MCXN_USBDCD(opaque);
    uint32_t v = value;

    if (off >= MCXN_USBDCD_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_STATUS:
        return;                              /* read-only */
    case R_CONTROL:
        /* Persist only IE/BC12 (and the current IF); the action bits are edges. */
        s->regs[R_CONTROL / 4] = (s->regs[R_CONTROL / 4] & CONTROL_IF) |
                                 (v & (CONTROL_IE | CONTROL_BC12));
        if (v & CONTROL_SR) {                /* soft reset: abandon the sequence */
            s->phase = 0;
            s->regs[R_STATUS / 4] = 0;
            s->regs[R_CONTROL / 4] &= ~CONTROL_IF;
        }
        if (v & CONTROL_IACK) {              /* ack this phase, then the HW advances */
            s->regs[R_CONTROL / 4] &= ~CONTROL_IF;
            if (s->regs[R_STATUS / 4] & STATUS_ACTIVE) {
                dcd_advance(s);
            }
        }
        if (v & CONTROL_START) {
            dcd_start(s);
        }
        dcd_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_usbdcd_ops = {
    .read = mcxn_usbdcd_read,
    .write = mcxn_usbdcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_usbdcd_reset(DeviceState *dev)
{
    MCXNUSBDCDState *s = MCXN_USBDCD(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->phase = 0;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_usbdcd_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBDCDState *s = MCXN_USBDCD(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_usbdcd_ops, s,
                          TYPE_MCXN_USBDCD, MCXN_USBDCD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property mcxn_usbdcd_props[] = {
    /* What is attached to the port: 0 none, 1 SDP, 2 CDP, 3 DCP.  Operator input,
     * the way the analog blocks are driven -- default NONE (nothing plugged in). */
    DEFINE_PROP_UINT8("charger", MCXNUSBDCDState, charger, MCXN_DCD_NONE),
};

static const VMStateDescription vmstate_mcxn_usbdcd = {
    .name = TYPE_MCXN_USBDCD,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBDCDState, MCXN_USBDCD_SIZE / 4),
        VMSTATE_UINT8(phase, MCXNUSBDCDState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_usbdcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_usbdcd_realize;
    device_class_set_legacy_reset(dc, mcxn_usbdcd_reset);
    dc->vmsd = &vmstate_mcxn_usbdcd;
    device_class_set_props(dc, mcxn_usbdcd_props);
}

static const TypeInfo mcxn_usbdcd_types[] = {
    {
        .name          = TYPE_MCXN_USBDCD,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUSBDCDState),
        .class_init    = mcxn_usbdcd_class_init,
    },
};

DEFINE_TYPES(mcxn_usbdcd_types)
