/*
 * NXP MCX N INPUTMUX (input multiplexer / trigger routing) — bring-up model.
 *
 * Permissive register-backed model.  Every INPUTMUX register is a routing
 * selector (__IO) or a write-only SET/CLR/TOG alias (__O); all reset to 0 and
 * have no externally-observable behavior in emulation, so storing and reading
 * back the written value is faithful.  Offsets from the MCXN947 CMSIS header
 * (INPUTMUX_Type, base 0x40006000, registers up to offset 0x7B8).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_inputmux.h"
#include "migration/vmstate.h"

static uint64_t mcxn_inputmux_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(opaque);

    return s->regs[offset / 4];
}

static void mcxn_inputmux_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(opaque);

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_inputmux_ops = {
    .read = mcxn_inputmux_read,
    .write = mcxn_inputmux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_inputmux_reset(DeviceState *dev)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_inputmux_realize(DeviceState *dev, Error **errp)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_inputmux_ops, s,
                          TYPE_MCXN_INPUTMUX, MCXN_INPUTMUX_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_inputmux = {
    .name = TYPE_MCXN_INPUTMUX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNInputMuxState, MCXN_INPUTMUX_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_inputmux_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_inputmux_realize;
    device_class_set_legacy_reset(dc, mcxn_inputmux_reset);
    dc->vmsd = &vmstate_mcxn_inputmux;
}

static const TypeInfo mcxn_inputmux_types[] = {
    {
        .name          = TYPE_MCXN_INPUTMUX,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNInputMuxState),
        .class_init    = mcxn_inputmux_class_init,
    },
};

DEFINE_TYPES(mcxn_inputmux_types)
