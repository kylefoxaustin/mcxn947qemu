/*
 * NXP MCX N TRDC (Trusted Resource Domain Controller) - bring-up model.
 *
 * Faithful register file with no access enforcement.  TRDC is a large
 * access-control/domain peripheral; this model exposes its MBC (Memory Block
 * Checker) configuration register file backed permissively so firmware can
 * program domain access policy and read it back.  The MBC_NSE_BLK_SET /
 * MBC_NSE_BLK_CLR / MBC_NSE_BLK_CLR_ALL registers are write-only set/clear
 * aliases that modify the indexed NonSecure-Enable word; they are accepted but
 * have no enforced effect (read back as 0).  All other MBC registers are
 * read/write.  Offsets from the MCXN947 CMSIS header (TRDC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_trdc.h"
#include "migration/vmstate.h"

/*
 * Register offsets within the single MBC_INDEX[0] block (array step 0x1CC).
 * MBC_NSE_BLK_SET/CLR/CLR_ALL are write-only.
 */
#define TRDC_MBC_NSE_BLK_SET      0x14  /* WO */
#define TRDC_MBC_NSE_BLK_CLR      0x18  /* WO */
#define TRDC_MBC_NSE_BLK_CLR_ALL  0x1C  /* WO */

static bool trdc_is_wo(hwaddr off)
{
    switch (off) {
    case TRDC_MBC_NSE_BLK_SET:
    case TRDC_MBC_NSE_BLK_CLR:
    case TRDC_MBC_NSE_BLK_CLR_ALL:
        return true;
    default:
        return false;
    }
}

static uint64_t mcxn_trdc_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNTRDCState *s = MCXN_TRDC(opaque);

    if (off >= MCXN_TRDC_SIZE) {
        return 0;
    }
    if (trdc_is_wo(off)) {
        return 0;        /* write-only set/clear registers */
    }
    return s->regs[off >> 2];
}

static void mcxn_trdc_write(void *opaque, hwaddr off, uint64_t value,
                            unsigned size)
{
    MCXNTRDCState *s = MCXN_TRDC(opaque);

    if (off >= MCXN_TRDC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    /* Write-only set/clear aliases: accepted, no enforced effect. */
    if (trdc_is_wo(off)) {
        return;
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_trdc_ops = {
    .read = mcxn_trdc_read,
    .write = mcxn_trdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_trdc_reset(DeviceState *dev)
{
    MCXNTRDCState *s = MCXN_TRDC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_trdc_realize(DeviceState *dev, Error **errp)
{
    MCXNTRDCState *s = MCXN_TRDC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_trdc_ops, s,
                          TYPE_MCXN_TRDC, MCXN_TRDC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_trdc = {
    .name = TYPE_MCXN_TRDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNTRDCState, MCXN_TRDC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_trdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_trdc_realize;
    device_class_set_legacy_reset(dc, mcxn_trdc_reset);
    dc->vmsd = &vmstate_mcxn_trdc;
}

static const TypeInfo mcxn_trdc_types[] = {
    {
        .name          = TYPE_MCXN_TRDC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNTRDCState),
        .class_init    = mcxn_trdc_class_init,
    },
};

DEFINE_TYPES(mcxn_trdc_types)
