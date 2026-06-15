/*
 * NXP MCX N PORT (pin mux / pin control) — bring-up stub.
 *
 * Permissive register-backed model: PCR[] and the other PORT registers are
 * stored and read back so firmware pin-mux/pull configuration completes.  Pin
 * muxing has no effect on the emulated GPIO/peripheral function.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_port.h"
#include "migration/vmstate.h"

#define PORT_VERID  0x00   /* RO */

#define PORT_VERID_VALUE 0x01000000u

static uint64_t mcxn_port_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNPortState *s = MCXN_PORT(opaque);

    if (offset == PORT_VERID) {
        return PORT_VERID_VALUE;
    }
    return s->regs[offset / 4];
}

static void mcxn_port_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNPortState *s = MCXN_PORT(opaque);

    if (offset == PORT_VERID) {
        return;  /* read-only */
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_port_ops = {
    .read = mcxn_port_read,
    .write = mcxn_port_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_port_reset(DeviceState *dev)
{
    MCXNPortState *s = MCXN_PORT(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_port_realize(DeviceState *dev, Error **errp)
{
    MCXNPortState *s = MCXN_PORT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_port_ops, s,
                          TYPE_MCXN_PORT, MCXN_PORT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_port = {
    .name = TYPE_MCXN_PORT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPortState, MCXN_PORT_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_port_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_port_realize;
    device_class_set_legacy_reset(dc, mcxn_port_reset);
    dc->vmsd = &vmstate_mcxn_port;
}

static const TypeInfo mcxn_port_types[] = {
    {
        .name          = TYPE_MCXN_PORT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPortState),
        .class_init    = mcxn_port_class_init,
    },
};

DEFINE_TYPES(mcxn_port_types)
