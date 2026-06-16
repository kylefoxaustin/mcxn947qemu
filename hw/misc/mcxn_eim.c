/*
 * NXP MCX N EIM (Error Injection Module) — register-accurate model.
 *
 * The EIM sits between the memory controller and the RAM arrays and lets
 * software inject ECC errors (single-bit and multi-bit) for fault testing.
 * Every EIM register is RW (CMSIS __IO) and resets to 0 (RM section 29.6); the
 * configuration registers (EIMCR, EICHEN) and the per-channel error-injection
 * descriptors (EICHD0..EICHD8, two words each) have no externally-observable
 * behavior in emulation, so storing and reading back the written value is
 * faithful.  No error is ever actually injected, so reporting reads "no error".
 *
 * Offsets from the MCXN947 CMSIS header (EIM_Type); registers EIMCR(0x0),
 * EICHEN(0x4) and EICHDn_WORD0/WORD1 at 0x100 + n*0x40.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_eim.h"
#include "migration/vmstate.h"

static uint64_t mcxn_eim_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNEIMState *s = MCXN_EIM(opaque);

    return s->regs[offset / 4];
}

static void mcxn_eim_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNEIMState *s = MCXN_EIM(opaque);

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_eim_ops = {
    .read = mcxn_eim_read,
    .write = mcxn_eim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_eim_reset(DeviceState *dev)
{
    MCXNEIMState *s = MCXN_EIM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_eim_realize(DeviceState *dev, Error **errp)
{
    MCXNEIMState *s = MCXN_EIM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_eim_ops, s,
                          TYPE_MCXN_EIM, MCXN_EIM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_eim = {
    .name = TYPE_MCXN_EIM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNEIMState, MCXN_EIM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_eim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_eim_realize;
    device_class_set_legacy_reset(dc, mcxn_eim_reset);
    dc->vmsd = &vmstate_mcxn_eim;
}

static const TypeInfo mcxn_eim_types[] = {
    {
        .name          = TYPE_MCXN_EIM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEIMState),
        .class_init    = mcxn_eim_class_init,
    },
};

DEFINE_TYPES(mcxn_eim_types)
