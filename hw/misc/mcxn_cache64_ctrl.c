/*
 * NXP MCX N CACHE64_CTRL (LPCAC / cache controller) — bring-up model.
 *
 * Controls the 64-bit local cache. The register block lives at offset 0x800:
 * CCR (cache control), CLCR (line control), CSAR (search address) and CCVR
 * (read/write value). Firmware enables the cache and issues maintenance
 * commands (invalidate/push all ways) by setting the relevant CCR bits together
 * with GO (bit 31); hardware clears the command bits and GO when the operation
 * finishes. The model completes instantly: GO and the per-way invalidate/push
 * bits read back clear, so a poll on GO never spins. Offsets/bits from the
 * MCXN947 CMSIS header (CACHE64_CTRL_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_cache64_ctrl.h"
#include "migration/vmstate.h"

/* Register offsets */
#define CACHE64_CCR   0x800   /* Cache Control */
#define CACHE64_CLCR  0x804   /* Cache Line Control */
#define CACHE64_CSAR  0x808   /* Cache Search Address */
#define CACHE64_CCVR  0x80C   /* Cache Read/Write Value */

/*
 * CCR command/GO bits that are self-clearing: once a maintenance command
 * completes the hardware clears these. The model completes instantly, so they
 * never read back set.
 */
#define CACHE64_CCR_INVW0   (1u << 24)
#define CACHE64_CCR_PUSHW0  (1u << 25)
#define CACHE64_CCR_INVW1   (1u << 26)
#define CACHE64_CCR_PUSHW1  (1u << 27)
#define CACHE64_CCR_GO      (1u << 31)
#define CACHE64_CCR_SELF_CLEAR \
    (CACHE64_CCR_INVW0 | CACHE64_CCR_PUSHW0 | \
     CACHE64_CCR_INVW1 | CACHE64_CCR_PUSHW1 | CACHE64_CCR_GO)

static uint64_t mcxn_cache64_ctrl_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNCache64CtrlState *s = MCXN_CACHE64_CTRL(opaque);
    uint32_t v = (off < MCXN_CACHE64_CTRL_SIZE) ? s->regs[off >> 2] : 0;

    if (off == CACHE64_CCR) {
        /* Maintenance commands complete instantly; GO/command bits read clear. */
        return v & ~CACHE64_CCR_SELF_CLEAR;
    }
    return v;
}

static void mcxn_cache64_ctrl_write(void *opaque, hwaddr off,
                                    uint64_t value, unsigned size)
{
    MCXNCache64CtrlState *s = MCXN_CACHE64_CTRL(opaque);

    if (off >= MCXN_CACHE64_CTRL_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    if (off == CACHE64_CCR) {
        /* Drop the self-clearing command bits; keep configuration bits. */
        s->regs[off >> 2] = (uint32_t)value & ~CACHE64_CCR_SELF_CLEAR;
        return;
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_cache64_ctrl_ops = {
    .read = mcxn_cache64_ctrl_read,
    .write = mcxn_cache64_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_cache64_ctrl_reset(DeviceState *dev)
{
    MCXNCache64CtrlState *s = MCXN_CACHE64_CTRL(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_cache64_ctrl_realize(DeviceState *dev, Error **errp)
{
    MCXNCache64CtrlState *s = MCXN_CACHE64_CTRL(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_cache64_ctrl_ops, s,
                          TYPE_MCXN_CACHE64_CTRL, MCXN_CACHE64_CTRL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_cache64_ctrl = {
    .name = TYPE_MCXN_CACHE64_CTRL,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNCache64CtrlState,
                             MCXN_CACHE64_CTRL_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_cache64_ctrl_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_cache64_ctrl_realize;
    device_class_set_legacy_reset(dc, mcxn_cache64_ctrl_reset);
    dc->vmsd = &vmstate_mcxn_cache64_ctrl;
}

static const TypeInfo mcxn_cache64_ctrl_types[] = {
    {
        .name          = TYPE_MCXN_CACHE64_CTRL,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCache64CtrlState),
        .class_init    = mcxn_cache64_ctrl_class_init,
    },
};

DEFINE_TYPES(mcxn_cache64_ctrl_types)
