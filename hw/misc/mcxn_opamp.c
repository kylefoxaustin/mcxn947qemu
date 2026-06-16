/*
 * NXP MCX N OPAMP (Operational Amplifier) — register-accurate model.
 *
 * One shared device type (OPAMP_Type) instantiated three times on the MCXN947
 * (OPAMP0/1/2).  The OPAMP is a pure analog block; QEMU does not simulate the
 * analog path, so a faithful register model (correct offsets, access types and
 * reset values) is the correct behaviour.
 *
 *   - VERID is read-only (CMSIS __I) and returns its constant version ID.
 *   - PARAM is read-only (CMSIS __I) and returns its constant parameter value.
 *   - OPAMP_CTR is read/write configuration (CMSIS __IO).  It has no
 *     settling/ready status bit of its own, so it is simply stored; an enabled
 *     amplifier is considered immediately ready in the model.
 *
 * Offsets/access-types from the MCXN947 CMSIS header (OPAMP_Type); reset values
 * from the MCX N Reference Manual (section 46.7.1, OPAMP memory map).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_opamp.h"
#include "migration/vmstate.h"

#define OPAMP_VERID      0x00   /* RO  Version ID */
#define OPAMP_PARAM      0x04   /* RO  Parameter */
#define OPAMP_CTR        0x08   /* RW  OPAMP Control */

/* Constant RO values (RM reset values). */
#define OPAMP_VERID_VALUE   0x00000000u
#define OPAMP_PARAM_VALUE   0x00000001u

/* Non-zero RM reset value for the backed control register. */
#define OPAMP_CTR_RESET     0x01000000u

static uint64_t mcxn_opamp_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNOPAMPState *s = MCXN_OPAMP(opaque);

    switch (offset) {
    case OPAMP_VERID:
        return OPAMP_VERID_VALUE;
    case OPAMP_PARAM:
        return OPAMP_PARAM_VALUE;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_opamp_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    MCXNOPAMPState *s = MCXN_OPAMP(opaque);

    switch (offset) {
    case OPAMP_VERID:
    case OPAMP_PARAM:
        /* Read-only registers: ignore writes. */
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_opamp_ops = {
    .read = mcxn_opamp_read,
    .write = mcxn_opamp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_opamp_reset(DeviceState *dev)
{
    MCXNOPAMPState *s = MCXN_OPAMP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[OPAMP_CTR / 4] = OPAMP_CTR_RESET;
}

static void mcxn_opamp_realize(DeviceState *dev, Error **errp)
{
    MCXNOPAMPState *s = MCXN_OPAMP(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_opamp_ops, s,
                          TYPE_MCXN_OPAMP, MCXN_OPAMP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_opamp = {
    .name = TYPE_MCXN_OPAMP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNOPAMPState, MCXN_OPAMP_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_opamp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_opamp_realize;
    device_class_set_legacy_reset(dc, mcxn_opamp_reset);
    dc->vmsd = &vmstate_mcxn_opamp;
}

static const TypeInfo mcxn_opamp_types[] = {
    {
        .name          = TYPE_MCXN_OPAMP,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNOPAMPState),
        .class_init    = mcxn_opamp_class_init,
    },
};

DEFINE_TYPES(mcxn_opamp_types)
