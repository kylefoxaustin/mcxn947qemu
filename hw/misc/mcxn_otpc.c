/*
 * NXP MCX N OTPC (OTP / fuse controller) — bring-up model.
 *
 * The OTPC reads and (on real silicon) programs the one-time-programmable fuse
 * array. Firmware triggers a read or shadow-reload via RWC/RLC and polls SR for
 * the operation to complete. This model completes instantly: the SR BUSY and
 * WR_REG_BUSY bits always read 0 (done) and no error/lock flags are set, so
 * init never spins. Fuse read data (RDATA) reads 0. VERID/PARAM and the various
 * lock/secure status registers read constant. Offsets/bits from the MCXN947
 * CMSIS header (OTPC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_otpc.h"
#include "migration/vmstate.h"

/* Register offsets */
#define OTPC_VERID       0x00   /* RO */
#define OTPC_PARAM       0x04   /* RO */
#define OTPC_SR          0x08   /* Status */
#define OTPC_RDATA       0x24   /* RO: read data */
#define OTPC_LOCK        0x200  /* RO */
#define OTPC_SECURE      0x204  /* RO */
#define OTPC_SECURE_INV  0x208  /* RO */
#define OTPC_DBG_KEY     0x20C  /* RO */

/* SR bits the driver polls; these must read complete/idle. */
#define OTPC_SR_BUSY         (1u << 0)
#define OTPC_SR_WR_REG_BUSY  (1u << 12)
#define OTPC_SR_BUSY_MASK    (OTPC_SR_BUSY | OTPC_SR_WR_REG_BUSY)

#define OTPC_VERID_VALUE  0x02000000u
/* PARAM: number of fuse words (capability indication). */
#define OTPC_PARAM_VALUE  0x00000080u

static uint64_t mcxn_otpc_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNOTPCState *s = MCXN_OTPC(opaque);

    switch (off) {
    case OTPC_VERID:
        return OTPC_VERID_VALUE;
    case OTPC_PARAM:
        return OTPC_PARAM_VALUE;
    case OTPC_SR:
        /* Operations complete instantly: never busy. */
        return s->regs[off >> 2] & ~OTPC_SR_BUSY_MASK;
    case OTPC_RDATA:
    case OTPC_LOCK:
    case OTPC_SECURE:
    case OTPC_SECURE_INV:
    case OTPC_DBG_KEY:
        /* Fuse data and lock/secure status read as unprogrammed/unlocked. */
        return 0;
    default:
        return (off < MCXN_OTPC_SIZE) ? s->regs[off >> 2] : 0;
    }
}

static void mcxn_otpc_write(void *opaque, hwaddr off,
                            uint64_t value, unsigned size)
{
    MCXNOTPCState *s = MCXN_OTPC(opaque);

    if (off >= MCXN_OTPC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case OTPC_VERID:
    case OTPC_PARAM:
    case OTPC_RDATA:
    case OTPC_LOCK:
    case OTPC_SECURE:
    case OTPC_SECURE_INV:
    case OTPC_DBG_KEY:
        return;  /* read-only */
    case OTPC_SR:
        /* SR flag bits are write-1-to-clear; busy bits never set. */
        s->regs[off >> 2] &= ~(uint32_t)value;
        return;
    default:
        s->regs[off >> 2] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_otpc_ops = {
    .read = mcxn_otpc_read,
    .write = mcxn_otpc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_otpc_reset(DeviceState *dev)
{
    MCXNOTPCState *s = MCXN_OTPC(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_otpc_realize(DeviceState *dev, Error **errp)
{
    MCXNOTPCState *s = MCXN_OTPC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_otpc_ops, s,
                          TYPE_MCXN_OTPC, MCXN_OTPC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_otpc = {
    .name = TYPE_MCXN_OTPC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNOTPCState, MCXN_OTPC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_otpc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_otpc_realize;
    device_class_set_legacy_reset(dc, mcxn_otpc_reset);
    dc->vmsd = &vmstate_mcxn_otpc;
}

static const TypeInfo mcxn_otpc_types[] = {
    {
        .name          = TYPE_MCXN_OTPC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNOTPCState),
        .class_init    = mcxn_otpc_class_init,
    },
};

DEFINE_TYPES(mcxn_otpc_types)
