/*
 * NXP MCX N VREF (Voltage Reference) — register-accurate model.
 *
 * Single instance on the MCXN947 (VREF0).  The voltage reference is a pure
 * analog block; QEMU does not simulate the bandgap, so a faithful register
 * model is the correct behaviour.
 *
 * The bring-up-critical detail is CSR[VREFST] (bit 31): firmware enables the
 * bandgap and then polls this "Internal HC Voltage Reference Stable" bit until
 * it reads 1 before continuing.  In the model the reference is always stable,
 * so reads of CSR force VREFST high; init never spins.
 *
 *   - VERID is read-only (CMSIS __I) and returns its constant version ID.
 *   - CSR is read/write (CMSIS __IO); the stored value is returned with
 *     VREFST forced set on read.  VREFST itself is hardware status, so writes
 *     to it are ignored.
 *   - UTRIM is read/write (CMSIS __IO) trim and is simply stored.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (VREF_Type); reset
 * values from the MCX N Reference Manual (section 45.6.1, VREF memory map).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_vref.h"
#include "migration/vmstate.h"

#define VREF_VERID    0x00   /* RO  Version ID */
#define VREF_CSR      0x08   /* RW  Control and Status */
#define VREF_UTRIM    0x10   /* RW  User Trim */

/* CSR[VREFST]: bandgap stable/ready status (hardware-controlled). */
#define VREF_CSR_VREFST   (1u << 31)

/* Constant RO value (RM reset value). */
#define VREF_VERID_VALUE  0x01000000u

static uint64_t mcxn_vref_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNVREFState *s = MCXN_VREF(opaque);

    switch (offset) {
    case VREF_VERID:
        return VREF_VERID_VALUE;
    case VREF_CSR:
        /*
         * The reference is always stable in the model: VREFST reads 1 so that
         * firmware polling CSR[VREFST] after enabling the bandgap proceeds.
         */
        return s->regs[VREF_CSR / 4] | VREF_CSR_VREFST;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_vref_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNVREFState *s = MCXN_VREF(opaque);

    switch (offset) {
    case VREF_VERID:
        /* Read-only register: ignore writes. */
        return;
    case VREF_CSR:
        /* VREFST is hardware status; do not let software set/clear it. */
        s->regs[VREF_CSR / 4] = value & ~VREF_CSR_VREFST;
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_vref_ops = {
    .read = mcxn_vref_read,
    .write = mcxn_vref_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_vref_reset(DeviceState *dev)
{
    MCXNVREFState *s = MCXN_VREF(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_vref_realize(DeviceState *dev, Error **errp)
{
    MCXNVREFState *s = MCXN_VREF(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_vref_ops, s,
                          TYPE_MCXN_VREF, MCXN_VREF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_vref = {
    .name = TYPE_MCXN_VREF,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNVREFState, MCXN_VREF_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_vref_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_vref_realize;
    device_class_set_legacy_reset(dc, mcxn_vref_reset);
    dc->vmsd = &vmstate_mcxn_vref;
}

static const TypeInfo mcxn_vref_types[] = {
    {
        .name          = TYPE_MCXN_VREF,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNVREFState),
        .class_init    = mcxn_vref_class_init,
    },
};

DEFINE_TYPES(mcxn_vref_types)
