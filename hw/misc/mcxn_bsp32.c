/*
 * NXP MCX N BSP32 (CoolFlux BSP32 coprocessor bus block) — bring-up model.
 *
 * The BSP32 block exposes the control interface to the CoolFlux DSP: program /
 * X / Y / mailbox memory offset registers, interrupt registers and the IVT
 * registers.  The model backs the read/write registers as plain storage.
 * SLEEP_MODE is read-only (__I) and reads back the awake/idle value (0).  The
 * INTERRUPTS_STATUS register is read/write storage; with no DSP modelled it
 * reads 0 (no pending interrupts).  Offsets from the MCXN947 CMSIS header
 * (BSP32_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_bsp32.h"
#include "migration/vmstate.h"

#define BSP32_OFFSET_PMEM           0x00
#define BSP32_OFFSET_XMEM           0x04
#define BSP32_OFFSET_YMEM           0x08
#define BSP32_OFFSET_MAILBOX        0x0C
#define BSP32_INTERRUPTS_EXTERNAL   0x10  /* WO */
#define BSP32_INTERRUPTS_STATUS     0x14
#define BSP32_CF_GATING_OVERRIDE    0x18
#define BSP32_IVT_OFFSET            0x1C
#define BSP32_SLEEP_MODE            0x20  /* RO */
#define BSP32_IVT0                  0x24
#define BSP32_IVT1                  0x28
#define BSP32_IVT2                  0x2C
#define BSP32_IVT3                  0x30
#define BSP32_IVT_DISABLE           0x34

static uint64_t mcxn_bsp32_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNBSP32State *s = MCXN_BSP32(opaque);

    if (off >= MCXN_BSP32_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off) {
    case BSP32_SLEEP_MODE:
        /* No DSP modelled: report awake/idle. */
        return 0;
    default:
        return s->regs[off >> 2];
    }
}

static void mcxn_bsp32_write(void *opaque, hwaddr off,
                             uint64_t value, unsigned size)
{
    MCXNBSP32State *s = MCXN_BSP32(opaque);

    if (off >= MCXN_BSP32_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (off == BSP32_SLEEP_MODE) {
        return;  /* read-only */
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_bsp32_ops = {
    .read = mcxn_bsp32_read,
    .write = mcxn_bsp32_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_bsp32_reset(DeviceState *dev)
{
    MCXNBSP32State *s = MCXN_BSP32(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_bsp32_realize(DeviceState *dev, Error **errp)
{
    MCXNBSP32State *s = MCXN_BSP32(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_bsp32_ops, s,
                          TYPE_MCXN_BSP32, MCXN_BSP32_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_bsp32 = {
    .name = TYPE_MCXN_BSP32,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNBSP32State, MCXN_BSP32_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_bsp32_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_bsp32_realize;
    device_class_set_legacy_reset(dc, mcxn_bsp32_reset);
    dc->vmsd = &vmstate_mcxn_bsp32;
}

static const TypeInfo mcxn_bsp32_types[] = {
    {
        .name          = TYPE_MCXN_BSP32,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNBSP32State),
        .class_init    = mcxn_bsp32_class_init,
    },
};

DEFINE_TYPES(mcxn_bsp32_types)
