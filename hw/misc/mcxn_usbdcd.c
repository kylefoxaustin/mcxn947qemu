/*
 * NXP MCX N USBDCD0 — USB Device Charger Detection register model.
 *
 * Register-accurate; no actual charger-detect sequencing.  The bits firmware
 * polls self-clear: CONTROL.SR (soft reset, bit 25) and CONTROL.START
 * (bit 24).  CONTROL.IACK (bit 0) is write-only ack that does not read back.
 * STATUS is read-only.  Offsets/bits from the MCXN947 CMSIS header
 * (USBDCD_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_usbdcd.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS USBDCD_Type). */
#define R_CONTROL          0x00
#define R_CLOCK            0x04
#define R_STATUS           0x08    /* RO */
#define R_SIGNAL_OVERRIDE  0x0C
#define R_TIMER0           0x10
#define R_TIMER1           0x14
#define R_TIMER2           0x18

#define CONTROL_IACK    (1u << 0)
#define CONTROL_START   (1u << 24)
#define CONTROL_SR      (1u << 25)

static uint64_t mcxn_usbdcd_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBDCDState *s = MCXN_USBDCD(opaque);

    switch (off) {
    case R_CONTROL:
        /* START and SR are self-clearing; IACK reads back 0. */
        return s->regs[R_CONTROL / 4] &
               ~(CONTROL_IACK | CONTROL_START | CONTROL_SR);
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
        return;                         /* read-only */
    case R_CONTROL:
        /* Store control config but never latch the self-clearing/ack bits. */
        s->regs[R_CONTROL / 4] =
            v & ~(CONTROL_IACK | CONTROL_START | CONTROL_SR);
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
}

static void mcxn_usbdcd_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBDCDState *s = MCXN_USBDCD(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_usbdcd_ops, s,
                          TYPE_MCXN_USBDCD, MCXN_USBDCD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_usbdcd = {
    .name = TYPE_MCXN_USBDCD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBDCDState, MCXN_USBDCD_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_usbdcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_usbdcd_realize;
    device_class_set_legacy_reset(dc, mcxn_usbdcd_reset);
    dc->vmsd = &vmstate_mcxn_usbdcd;
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
