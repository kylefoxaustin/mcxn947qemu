/*
 * NXP MCX N PKC (Public Key Crypto) - bring-up model.
 *
 * Faithful register file, no real crypto.  Firmware programs the parameter
 * sets and triggers an operation, then polls PKC_STATUS.ACTIV (the busy bit)
 * for completion.  Here the model reports the engine permanently idle:
 * PKC_STATUS.ACTIV reads 0 so any "wait for done" loop completes, and
 * PKC_ACCESS_ERR / PKC_INT_STATUS read benign (no error/interrupt pending).
 * PKC_SOFT_RST, the access-error clear and the interrupt enable/status
 * set/clear registers are write-only and ignored.  Read-only version and
 * module-ID registers return constants.  Offsets/bits from the MCXN947 CMSIS
 * header (PKC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_pkc.h"
#include "migration/vmstate.h"

/* Register offsets (PKC_Type). */
#define PKC_STATUS           0x000  /* RO */
#define PKC_CTRL             0x004
#define PKC_CFG              0x008
#define PKC_MODE1            0x010
#define PKC_XYPTR1           0x014
#define PKC_ZRPTR1           0x018
#define PKC_LEN1             0x01C
#define PKC_MODE2            0x020
#define PKC_XYPTR2           0x024
#define PKC_ZRPTR2           0x028
#define PKC_LEN2             0x02C
#define PKC_UPTR             0x040
#define PKC_UPTRT            0x044
#define PKC_ULEN             0x048
#define PKC_MCDATA           0x050
#define PKC_VERSION          0x060  /* RO */
#define PKC_SOFT_RST         0xFB0  /* WO */
#define PKC_ACCESS_ERR       0xFC0  /* RO */
#define PKC_ACCESS_ERR_CLR   0xFC4  /* WO */
#define PKC_INT_CLR_ENABLE   0xFD8  /* WO */
#define PKC_INT_SET_ENABLE   0xFDC  /* WO */
#define PKC_INT_STATUS       0xFE0  /* RO */
#define PKC_INT_ENABLE       0xFE4  /* RO */
#define PKC_INT_CLR_STATUS   0xFE8  /* WO */
#define PKC_INT_SET_STATUS   0xFEC  /* WO */
#define PKC_MODULE_ID        0xFFC  /* RO */

/* PKC_STATUS field masks. */
#define PKC_STATUS_ACTIV   (1u << 0)   /* busy */
#define PKC_STATUS_CARRY   (1u << 1)
#define PKC_STATUS_ZERO    (1u << 2)
#define PKC_STATUS_GOANY   (1u << 3)

/* Idle status: not active (not busy). */
#define PKC_STATUS_IDLE  0u

/* Read-only constants.  Unconfirmed against RM. */
#define PKC_VERSION_VALUE    0x000000C0u   /* 2 parameter sets, MUL size base */
#define PKC_MODULE_ID_VALUE  0x00000000u

static bool pkc_is_ro(hwaddr off)
{
    switch (off) {
    case PKC_STATUS:
    case PKC_VERSION:
    case PKC_ACCESS_ERR:
    case PKC_INT_STATUS:
    case PKC_INT_ENABLE:
    case PKC_MODULE_ID:
        return true;
    default:
        return false;
    }
}

static bool pkc_is_wo(hwaddr off)
{
    switch (off) {
    case PKC_SOFT_RST:
    case PKC_ACCESS_ERR_CLR:
    case PKC_INT_CLR_ENABLE:
    case PKC_INT_SET_ENABLE:
    case PKC_INT_CLR_STATUS:
    case PKC_INT_SET_STATUS:
        return true;
    default:
        return false;
    }
}

static uint64_t mcxn_pkc_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNPKCState *s = MCXN_PKC(opaque);
    uint32_t v = (off < MCXN_PKC_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case PKC_STATUS:
        /* Operations complete instantly: never active/busy. */
        return PKC_STATUS_IDLE;
    case PKC_VERSION:
        return PKC_VERSION_VALUE;
    case PKC_MODULE_ID:
        return PKC_MODULE_ID_VALUE;
    case PKC_ACCESS_ERR:
        return 0;        /* no access error pending */
    case PKC_INT_STATUS:
        return 0;        /* no interrupt pending */
    default:
        if (pkc_is_wo(off)) {
            return 0;
        }
        return v;
    }
}

static void mcxn_pkc_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    MCXNPKCState *s = MCXN_PKC(opaque);

    if (off >= MCXN_PKC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (pkc_is_ro(off)) {
        return;          /* read-only registers ignore writes */
    }
    if (pkc_is_wo(off)) {
        return;          /* write-only side effects: no observable state */
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_pkc_ops = {
    .read = mcxn_pkc_read,
    .write = mcxn_pkc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_pkc_reset(DeviceState *dev)
{
    MCXNPKCState *s = MCXN_PKC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_pkc_realize(DeviceState *dev, Error **errp)
{
    MCXNPKCState *s = MCXN_PKC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_pkc_ops, s,
                          TYPE_MCXN_PKC, MCXN_PKC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_pkc = {
    .name = TYPE_MCXN_PKC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPKCState, MCXN_PKC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_pkc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_pkc_realize;
    device_class_set_legacy_reset(dc, mcxn_pkc_reset);
    dc->vmsd = &vmstate_mcxn_pkc;
}

static const TypeInfo mcxn_pkc_types[] = {
    {
        .name          = TYPE_MCXN_PKC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPKCState),
        .class_init    = mcxn_pkc_class_init,
    },
};

DEFINE_TYPES(mcxn_pkc_types)
