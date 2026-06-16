/*
 * NXP MCX N UTICK (Micro-Tick Timer) — bring-up model.
 *
 * The micro-tick timer counts a one-shot/repeat delay programmed via CTRL and
 * raises STAT.INTR (bit 0) when it expires; STAT.ACTIVE (bit 1) indicates the
 * timer is still running.  With no timer modelled, STAT reads idle (ACTIVE
 * clear) and INTR is write-1-to-clear, so a driver that programs a delay and
 * polls for completion never spins forever waiting on a stuck ACTIVE bit.  CFG
 * is plain storage, CAPCLR is write-only and the CAP[4] capture registers are
 * read-only.  Offsets from the MCXN947 CMSIS header (UTICK_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_utick.h"
#include "migration/vmstate.h"

#define UTICK_CTRL    0x00
#define UTICK_STAT    0x04  /* W1C status */
#define UTICK_CFG     0x08
#define UTICK_CAPCLR  0x0C  /* WO */
#define UTICK_CAP0    0x10  /* RO, CAP[4] through 0x1C */
#define UTICK_CAP3    0x1C

#define UTICK_STAT_INTR    (1u << 0)  /* W1C tick interrupt flag */
#define UTICK_STAT_ACTIVE  (1u << 1)  /* timer running */

static uint64_t mcxn_utick_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUTICKState *s = MCXN_UTICK(opaque);

    if (off >= MCXN_UTICK_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off) {
    case UTICK_STAT:
        /* No timer modelled: idle (not active), no pending tick. */
        return 0;
    case UTICK_CAPCLR:
        return 0;  /* write-only */
    default:
        return s->regs[off >> 2];
    }
}

static void mcxn_utick_write(void *opaque, hwaddr off,
                             uint64_t value, unsigned size)
{
    MCXNUTICKState *s = MCXN_UTICK(opaque);

    if (off >= MCXN_UTICK_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case UTICK_STAT:
        /* Write-1-to-clear; nothing is ever set, so this is a no-op. */
        break;
    case UTICK_CAP0:
    case UTICK_CAP0 + 0x4:
    case UTICK_CAP0 + 0x8:
    case UTICK_CAP3:
        break;  /* read-only capture registers */
    default:
        s->regs[off >> 2] = value;
        break;
    }
}

static const MemoryRegionOps mcxn_utick_ops = {
    .read = mcxn_utick_read,
    .write = mcxn_utick_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_utick_reset(DeviceState *dev)
{
    MCXNUTICKState *s = MCXN_UTICK(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_utick_realize(DeviceState *dev, Error **errp)
{
    MCXNUTICKState *s = MCXN_UTICK(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_utick_ops, s,
                          TYPE_MCXN_UTICK, MCXN_UTICK_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_utick = {
    .name = TYPE_MCXN_UTICK,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUTICKState, MCXN_UTICK_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_utick_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_utick_realize;
    device_class_set_legacy_reset(dc, mcxn_utick_reset);
    dc->vmsd = &vmstate_mcxn_utick;
}

static const TypeInfo mcxn_utick_types[] = {
    {
        .name          = TYPE_MCXN_UTICK,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUTICKState),
        .class_init    = mcxn_utick_class_init,
    },
};

DEFINE_TYPES(mcxn_utick_types)
