/*
 * NXP MCX N generic peripheral stub — see mcxn_stub.h.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_stub.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

static uint64_t mcxn_stub_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNStubState *s = MCXN_STUB(opaque);

    if (offset + 4 > s->size) {
        return 0;
    }
    return s->regs[offset / 4];
}

static void mcxn_stub_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNStubState *s = MCXN_STUB(opaque);

    if (offset + 4 > s->size) {
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_stub_ops = {
    .read = mcxn_stub_read,
    .write = mcxn_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_stub_reset(DeviceState *dev)
{
    MCXNStubState *s = MCXN_STUB(dev);

    if (s->regs) {
        memset(s->regs, 0, s->size);
    }
}

static void mcxn_stub_realize(DeviceState *dev, Error **errp)
{
    MCXNStubState *s = MCXN_STUB(dev);

    if (s->size == 0) {
        s->size = 0x1000;
    }
    s->regs = g_malloc0(s->size);
    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_stub_ops, s,
                          s->blkname ? s->blkname : TYPE_MCXN_STUB, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property mcxn_stub_properties[] = {
    DEFINE_PROP_STRING("blkname", MCXNStubState, blkname),
    DEFINE_PROP_UINT64("size", MCXNStubState, size, 0x1000),
};

static void mcxn_stub_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_stub_realize;
    device_class_set_legacy_reset(dc, mcxn_stub_reset);
    device_class_set_props(dc, mcxn_stub_properties);
    dc->user_creatable = false;
}

static const TypeInfo mcxn_stub_types[] = {
    {
        .name          = TYPE_MCXN_STUB,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNStubState),
        .class_init    = mcxn_stub_class_init,
    },
};

DEFINE_TYPES(mcxn_stub_types)
