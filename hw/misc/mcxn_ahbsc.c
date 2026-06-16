/*
 * NXP MCX N AHBSC (AHB Secure Controller) — bring-up model.
 *
 * The AHB secure controller holds the secure access-rule register file (flash,
 * ROM, RAM and bus-bridge memory rules) together with the master security-level
 * and CPU lock registers.  Firmware programs these during secure init; the
 * model backs every rule register as plain read/write storage so the values
 * read back as written.  The SEC_VIO_ADDR/SEC_VIO_MISC_INFO violation-log
 * registers are read-only (__I in the CMSIS header) and report no violation.
 * Offsets from the MCXN947 CMSIS header (AHBSC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_ahbsc.h"
#include "migration/vmstate.h"

/* Read-only (__I) security-violation log windows. */
#define AHBSC_SEC_VIO_ADDR_BASE       0xE00   /* SEC_VIO_ADDR[32]      */
#define AHBSC_SEC_VIO_ADDR_END        0xE7C
#define AHBSC_SEC_VIO_MISC_INFO_BASE  0xE80   /* SEC_VIO_MISC_INFO[32] */
#define AHBSC_SEC_VIO_MISC_INFO_END   0xEFC

static bool ahbsc_readonly(hwaddr off)
{
    return (off >= AHBSC_SEC_VIO_ADDR_BASE &&
            off <= AHBSC_SEC_VIO_MISC_INFO_END);
}

static uint64_t mcxn_ahbsc_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNAHBSCState *s = MCXN_AHBSC(opaque);

    if (off >= MCXN_AHBSC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }
    return s->regs[off >> 2];
}

static void mcxn_ahbsc_write(void *opaque, hwaddr off,
                             uint64_t value, unsigned size)
{
    MCXNAHBSCState *s = MCXN_AHBSC(opaque);

    if (off >= MCXN_AHBSC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (ahbsc_readonly(off)) {
        return;  /* violation log is read-only */
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_ahbsc_ops = {
    .read = mcxn_ahbsc_read,
    .write = mcxn_ahbsc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_ahbsc_reset(DeviceState *dev)
{
    MCXNAHBSCState *s = MCXN_AHBSC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_ahbsc_realize(DeviceState *dev, Error **errp)
{
    MCXNAHBSCState *s = MCXN_AHBSC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_ahbsc_ops, s,
                          TYPE_MCXN_AHBSC, MCXN_AHBSC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_ahbsc = {
    .name = TYPE_MCXN_AHBSC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNAHBSCState, MCXN_AHBSC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_ahbsc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_ahbsc_realize;
    device_class_set_legacy_reset(dc, mcxn_ahbsc_reset);
    dc->vmsd = &vmstate_mcxn_ahbsc;
}

static const TypeInfo mcxn_ahbsc_types[] = {
    {
        .name          = TYPE_MCXN_AHBSC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNAHBSCState),
        .class_init    = mcxn_ahbsc_class_init,
    },
};

DEFINE_TYPES(mcxn_ahbsc_types)
