/*
 * NXP MCX N PINT (Pin Interrupt and Pattern Match) — bring-up model.
 *
 * Models the pin-interrupt register file enough for driver init to program and
 * read back interrupt enables.  The IENR (rising) and IENF (falling) enable
 * registers have write-only set/clear aliases: writing a 1 to SIENR/SIENF sets
 * the matching IENR/IENF bit, writing a 1 to CIENR/CIENF clears it.  The IST
 * status register is write-1-to-clear and, with no pin sources modelled, always
 * reads 0 (no pending pin interrupts).  RISE/FALL are write-1-to-clear edge-
 * detect registers and read 0.  ISEL and the pattern-match registers are plain
 * read/write storage.  Offsets from the MCXN947 CMSIS header (PINT_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_pint.h"
#include "migration/vmstate.h"

#define PINT_ISEL    0x00
#define PINT_IENR    0x04
#define PINT_SIENR   0x08  /* WO: set bits in IENR  */
#define PINT_CIENR   0x0C  /* WO: clear bits in IENR */
#define PINT_IENF    0x10
#define PINT_SIENF   0x14  /* WO: set bits in IENF  */
#define PINT_CIENF   0x18  /* WO: clear bits in IENF */
#define PINT_RISE    0x1C  /* W1C edge detect */
#define PINT_FALL    0x20  /* W1C edge detect */
#define PINT_IST     0x24  /* W1C status      */
#define PINT_PMCTRL  0x28
#define PINT_PMSRC   0x2C
#define PINT_PMCFG   0x30

static uint64_t mcxn_pint_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNPINTState *s = MCXN_PINT(opaque);

    if (off >= MCXN_PINT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off) {
    case PINT_SIENR:
    case PINT_CIENR:
    case PINT_SIENF:
    case PINT_CIENF:
        /* Write-only set/clear aliases read as 0. */
        return 0;
    case PINT_RISE:
    case PINT_FALL:
    case PINT_IST:
        /* No pin sources modelled: no edges captured, no status pending. */
        return 0;
    default:
        return s->regs[off >> 2];
    }
}

static void mcxn_pint_write(void *opaque, hwaddr off,
                            uint64_t value, unsigned size)
{
    MCXNPINTState *s = MCXN_PINT(opaque);
    uint32_t v = value;

    if (off >= MCXN_PINT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case PINT_SIENR:
        s->regs[PINT_IENR / 4] |= v;
        break;
    case PINT_CIENR:
        s->regs[PINT_IENR / 4] &= ~v;
        break;
    case PINT_SIENF:
        s->regs[PINT_IENF / 4] |= v;
        break;
    case PINT_CIENF:
        s->regs[PINT_IENF / 4] &= ~v;
        break;
    case PINT_RISE:
    case PINT_FALL:
    case PINT_IST:
        /* Write-1-to-clear; nothing is ever set, so this is a no-op. */
        break;
    default:
        s->regs[off >> 2] = v;
        break;
    }
}

static const MemoryRegionOps mcxn_pint_ops = {
    .read = mcxn_pint_read,
    .write = mcxn_pint_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_pint_reset(DeviceState *dev)
{
    MCXNPINTState *s = MCXN_PINT(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_pint_realize(DeviceState *dev, Error **errp)
{
    MCXNPINTState *s = MCXN_PINT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_pint_ops, s,
                          TYPE_MCXN_PINT, MCXN_PINT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_pint = {
    .name = TYPE_MCXN_PINT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPINTState, MCXN_PINT_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_pint_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_pint_realize;
    device_class_set_legacy_reset(dc, mcxn_pint_reset);
    dc->vmsd = &vmstate_mcxn_pint;
}

static const TypeInfo mcxn_pint_types[] = {
    {
        .name          = TYPE_MCXN_PINT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPINTState),
        .class_init    = mcxn_pint_class_init,
    },
};

DEFINE_TYPES(mcxn_pint_types)
