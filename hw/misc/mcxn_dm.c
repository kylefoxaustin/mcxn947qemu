/*
 * NXP MCX N DM (Debug Mailbox) — bring-up model.
 *
 * The Debug Mailbox lets an external debugger exchange command/response words
 * with the on-chip ROM.  Firmware/host drivers issue a request through REQUEST
 * and poll CSW.REQ_PENDING (bit 1) until the mailbox handler has consumed it.
 * With no mailbox handler modelled, REQ_PENDING is reported clear (request
 * already serviced) so any polling loop completes immediately.  REQUEST and
 * RETURN are plain read/write storage.  ID is read-only (__I).  Offsets from
 * the MCXN947 CMSIS header (DM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_dm.h"
#include "migration/vmstate.h"

#define DM_CSW       0x00
#define DM_REQUEST   0x04
#define DM_RETURN    0x08
#define DM_ID        0xFC  /* RO */

#define DM_CSW_REQ_PENDING  (1u << 1)  /* busy: request not yet serviced */

static uint64_t mcxn_dm_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNDMState *s = MCXN_DM(opaque);

    if (off >= MCXN_DM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off) {
    case DM_CSW:
        /* Requests are serviced instantly: never report pending/busy. */
        return s->regs[DM_CSW / 4] & ~DM_CSW_REQ_PENDING;
    case DM_ID:
        return s->regs[DM_ID / 4];
    default:
        return s->regs[off >> 2];
    }
}

static void mcxn_dm_write(void *opaque, hwaddr off,
                          uint64_t value, unsigned size)
{
    MCXNDMState *s = MCXN_DM(opaque);

    if (off >= MCXN_DM_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (off == DM_ID) {
        return;  /* read-only */
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_dm_ops = {
    .read = mcxn_dm_read,
    .write = mcxn_dm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_dm_reset(DeviceState *dev)
{
    MCXNDMState *s = MCXN_DM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_dm_realize(DeviceState *dev, Error **errp)
{
    MCXNDMState *s = MCXN_DM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_dm_ops, s,
                          TYPE_MCXN_DM, MCXN_DM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_dm = {
    .name = TYPE_MCXN_DM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNDMState, MCXN_DM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_dm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_dm_realize;
    device_class_set_legacy_reset(dc, mcxn_dm_reset);
    dc->vmsd = &vmstate_mcxn_dm;
}

static const TypeInfo mcxn_dm_types[] = {
    {
        .name          = TYPE_MCXN_DM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNDMState),
        .class_init    = mcxn_dm_class_init,
    },
};

DEFINE_TYPES(mcxn_dm_types)
