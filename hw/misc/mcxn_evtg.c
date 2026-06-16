/*
 * NXP MCX N EVTG (Event Generator) — bring-up model.
 *
 * The EVTG combines peripheral events through configurable AOI boolean logic
 * and output filters.  It is a pure configuration block: every register is an
 * __IO config field (the per-instance EVTG_CTRL is control/status but carries
 * no hardware-set status bits), all reset to 0.  A permissive register-backed
 * model that stores and reads back written values is therefore faithful.
 * Offsets from the MCXN947 CMSIS header (EVTG_Type: 4 instances x 0x10).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_evtg.h"
#include "migration/vmstate.h"

static uint64_t mcxn_evtg_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNEvtgState *s = MCXN_EVTG(opaque);

    return s->regs[offset / 4];
}

static void mcxn_evtg_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNEvtgState *s = MCXN_EVTG(opaque);

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_evtg_ops = {
    .read = mcxn_evtg_read,
    .write = mcxn_evtg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_evtg_reset(DeviceState *dev)
{
    MCXNEvtgState *s = MCXN_EVTG(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_evtg_realize(DeviceState *dev, Error **errp)
{
    MCXNEvtgState *s = MCXN_EVTG(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_evtg_ops, s,
                          TYPE_MCXN_EVTG, MCXN_EVTG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_evtg = {
    .name = TYPE_MCXN_EVTG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNEvtgState, MCXN_EVTG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_evtg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_evtg_realize;
    device_class_set_legacy_reset(dc, mcxn_evtg_reset);
    dc->vmsd = &vmstate_mcxn_evtg;
}

static const TypeInfo mcxn_evtg_types[] = {
    {
        .name          = TYPE_MCXN_EVTG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEvtgState),
        .class_init    = mcxn_evtg_class_init,
    },
};

DEFINE_TYPES(mcxn_evtg_types)
