/*
 * NXP MCX N WUU (Wake-Up Unit) — bring-up model.
 *
 * The WUU selects which external pins and on-chip modules can wake the SoC from
 * low-power modes. This model is a register file: enable/config registers are
 * stored, VERID/PARAM read constant, and the pin-flag (PF) register is
 * write-1-to-clear and reads 0 (no pending wake events) so driver init does not
 * see spurious wakes. Offsets/bits from the MCXN947 CMSIS header (WUU_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_wuu.h"
#include "migration/vmstate.h"

/* Register offsets */
#define WUU_VERID  0x00   /* RO */
#define WUU_PARAM  0x04   /* RO */
#define WUU_PF     0x20   /* Pin Flag (W1C) */

#define WUU_VERID_VALUE  0x02000000u
/* PARAM: 8 pins, 8 modules, 4 DMA channels, 8 filters (reported capability). */
#define WUU_PARAM_VALUE  0x08080408u

static uint64_t mcxn_wuu_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNWUUState *s = MCXN_WUU(opaque);

    switch (off) {
    case WUU_VERID:
        return WUU_VERID_VALUE;
    case WUU_PARAM:
        return WUU_PARAM_VALUE;
    case WUU_PF:
        /* No pending wake-up events in the model. */
        return 0;
    default:
        return (off < MCXN_WUU_SIZE) ? s->regs[off >> 2] : 0;
    }
}

static void mcxn_wuu_write(void *opaque, hwaddr off,
                           uint64_t value, unsigned size)
{
    MCXNWUUState *s = MCXN_WUU(opaque);

    if (off >= MCXN_WUU_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case WUU_VERID:
    case WUU_PARAM:
        return;  /* read-only */
    case WUU_PF:
        /* Write-1-to-clear; flags are never set in the model. */
        s->regs[off >> 2] &= ~(uint32_t)value;
        return;
    default:
        s->regs[off >> 2] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_wuu_ops = {
    .read = mcxn_wuu_read,
    .write = mcxn_wuu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_wuu_reset(DeviceState *dev)
{
    MCXNWUUState *s = MCXN_WUU(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_wuu_realize(DeviceState *dev, Error **errp)
{
    MCXNWUUState *s = MCXN_WUU(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_wuu_ops, s,
                          TYPE_MCXN_WUU, MCXN_WUU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_wuu = {
    .name = TYPE_MCXN_WUU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNWUUState, MCXN_WUU_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_wuu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_wuu_realize;
    device_class_set_legacy_reset(dc, mcxn_wuu_reset);
    dc->vmsd = &vmstate_mcxn_wuu;
}

static const TypeInfo mcxn_wuu_types[] = {
    {
        .name          = TYPE_MCXN_WUU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNWUUState),
        .class_init    = mcxn_wuu_class_init,
    },
};

DEFINE_TYPES(mcxn_wuu_types)
