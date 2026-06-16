/*
 * NXP MCX N PLU (Programmable Logic Unit) — bring-up model.
 *
 * The PLU implements user-defined combinatorial logic via 26 LUTs.  It is a
 * configuration block with no externally-observable behavior in emulation, so
 * a permissive register-backed model is faithful.  All registers are __IO
 * config that reset to 0 and are stored/read back, except OUTPUTS (0x900) which
 * is __I read-only: writes are ignored and it reads its (reset 0) state since
 * the emulated LUT array drives no real signals.  Offsets from the MCXN947
 * CMSIS header (PLU_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_plu.h"
#include "migration/vmstate.h"

#define PLU_OUTPUTS 0x900   /* RO */

static uint64_t mcxn_plu_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNPluState *s = MCXN_PLU(opaque);

    return s->regs[offset / 4];
}

static void mcxn_plu_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNPluState *s = MCXN_PLU(opaque);

    if (offset == PLU_OUTPUTS) {
        return;  /* read-only */
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_plu_ops = {
    .read = mcxn_plu_read,
    .write = mcxn_plu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_plu_reset(DeviceState *dev)
{
    MCXNPluState *s = MCXN_PLU(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_plu_realize(DeviceState *dev, Error **errp)
{
    MCXNPluState *s = MCXN_PLU(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_plu_ops, s,
                          TYPE_MCXN_PLU, MCXN_PLU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_plu = {
    .name = TYPE_MCXN_PLU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPluState, MCXN_PLU_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_plu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_plu_realize;
    device_class_set_legacy_reset(dc, mcxn_plu_reset);
    dc->vmsd = &vmstate_mcxn_plu;
}

static const TypeInfo mcxn_plu_types[] = {
    {
        .name          = TYPE_MCXN_PLU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPluState),
        .class_init    = mcxn_plu_class_init,
    },
};

DEFINE_TYPES(mcxn_plu_types)
