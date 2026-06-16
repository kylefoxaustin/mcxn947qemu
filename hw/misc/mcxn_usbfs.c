/*
 * NXP MCX N USBFS0 — USB Full-Speed OTG (device/host) register model.
 *
 * Register-accurate; no actual USB transfers.  Status semantics that firmware
 * polls during reset/init are honoured: CTL.RESET (bit 1) and
 * USBTRC0.USBRESET (bit 7) self-clear, and the W1C interrupt-status registers
 * (ISTAT, ERRSTAT, OTGISTAT) clear written-one bits.  Capability/ID registers
 * (PERID, IDCOMP, REV, ADDINFO) are read-only.  Offsets/bits from the MCXN947
 * CMSIS header (USB_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_usbfs.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

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
#define R_USBTRC0   0x10C

/* Bit fields. */
#define CTL_RESET           (1u << 1)   /* USB_CTL_RESET */
#define USBTRC0_USBRESET    (1u << 7)   /* USB_USBTRC0_USBRESET, self-clearing */

/* Reset value of PERID for this Freescale/NXP USB-FS IP is 0x04. */
#define PERID_VALUE     0x04
#define IDCOMP_VALUE    0xFB        /* ones-complement of PERID */
#define REV_VALUE       0x33        /* revision, RM-reported */

static void usbfs_update_irq(MCXNUSBFSState *s)
{
    bool active =
        ((s->regs[R_ISTAT / 4]    & s->regs[R_INTEN / 4])  & 0xFF) ||
        ((s->regs[R_ERRSTAT / 4]  & s->regs[R_ERREN / 4])  & 0xFF) ||
        ((s->regs[R_OTGISTAT / 4] & s->regs[R_OTGICR / 4]) & 0xFF);

    qemu_set_irq(s->irq, active);
}

static uint64_t mcxn_usbfs_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBFSState *s = MCXN_USBFS(opaque);

    switch (off) {
    case R_PERID:
        return PERID_VALUE;
    case R_IDCOMP:
        return IDCOMP_VALUE;
    case R_REV:
        return REV_VALUE;
    case R_CTL:
        /* RESET is self-clearing: never reads back set. */
        return s->regs[R_CTL / 4] & ~CTL_RESET;
    case R_USBTRC0:
        /* USBRESET is self-clearing. */
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
        return;                         /* read-only */
    case R_OTGISTAT:
    case R_ISTAT:
    case R_ERRSTAT:
        s->regs[off >> 2] &= ~(v & 0xFF);   /* write-1-to-clear */
        usbfs_update_irq(s);
        return;
    case R_CTL:
        /* RESET self-clears: store everything except that bit. */
        s->regs[off >> 2] = v & ~CTL_RESET;
        return;
    case R_USBTRC0:
        /* USBRESET self-clears. */
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
}

static void mcxn_usbfs_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBFSState *s = MCXN_USBFS(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_usbfs_ops, s,
                          TYPE_MCXN_USBFS, MCXN_USBFS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_usbfs = {
    .name = TYPE_MCXN_USBFS,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBFSState, MCXN_USBFS_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_usbfs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_usbfs_realize;
    device_class_set_legacy_reset(dc, mcxn_usbfs_reset);
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
