/*
 * NXP MCX N ELS (EdgeLock Secure subsystem, S50) - bring-up model.
 *
 * Faithful register file, no real crypto.  Firmware issues an ELS command via
 * ELS_CTRL and then polls ELS_STATUS for the BUSY bit to clear (and inspects
 * the error flag) before reading results.  Here the model reports the engine
 * as permanently idle/done: ELS_STATUS.ELS_BUSY and DTRNG_BUSY read 0, the
 * ERR flag reads 0, and the PRNG/DRBG ready bits read ready so entropy and
 * "wait for ELS done" loops complete.  ELS_INT_STATUS_CLR / ELS_ERR_STATUS_CLR
 * are write-only status-clear registers and are ignored.  Read-only ID/version
 * and keystore-status registers return constants.  Offsets/bits from the
 * MCXN947 CMSIS header (S50_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_els.h"
#include "migration/vmstate.h"

/* Register offsets (S50_Type). */
#define ELS_STATUS          0x00  /* RO: status */
#define ELS_CTRL            0x04
#define ELS_CMDCFG0         0x08
#define ELS_CFG             0x0C
#define ELS_KIDX0           0x10
#define ELS_KIDX1           0x14
#define ELS_KPROPIN         0x18
#define ELS_DMA_SRC0        0x20
#define ELS_DMA_SRC0_LEN    0x24
#define ELS_DMA_SRC1        0x28
#define ELS_DMA_SRC2        0x30
#define ELS_DMA_SRC2_LEN    0x34
#define ELS_DMA_RES0        0x38
#define ELS_DMA_RES0_LEN    0x3C
#define ELS_INT_ENABLE      0x40
#define ELS_INT_STATUS_CLR  0x44  /* WO */
#define ELS_INT_STATUS_SET  0x48  /* WO */
#define ELS_ERR_STATUS      0x4C  /* RO */
#define ELS_ERR_STATUS_CLR  0x50  /* WO */
#define ELS_VERSION         0x54  /* RO */
#define ELS_PRNG_DATOUT     0x5C  /* RO */
#define ELS_CMDCRC_CTRL     0x60
#define ELS_CMDCRC          0x64  /* RO */
#define ELS_SESSION_ID      0x68
#define ELS_DMA_FIN_ADDR    0x70  /* RO */
#define ELS_MASTER_ID       0x74
#define ELS_KIDX2           0x78
#define ELS_KS0             0x150 /* RO: keystore status 0 */
#define ELS_KS19            0x19C /* RO: keystore status 19 */

/* ELS_STATUS field masks. */
#define ELS_STATUS_ELS_BUSY     (1u << 0)
#define ELS_STATUS_ELS_IRQ      (1u << 1)
#define ELS_STATUS_ELS_ERR      (1u << 2)
#define ELS_STATUS_PRNG_RDY     (1u << 3)
#define ELS_STATUS_DTRNG_BUSY   (1u << 10)

/*
 * Idle/done value reported by ELS_STATUS: not busy, no error/IRQ pending,
 * PRNG ready.  DRBG entropy level reports max (0x300) so the entropy-ready
 * checks pass.
 */
#define ELS_STATUS_IDLE  (ELS_STATUS_PRNG_RDY | 0x300u)

/* Version register value (X.Y1.Y2.Z = 1.0.0.0). Unconfirmed against RM. */
#define ELS_VERSION_VALUE  0x00001000u

/* Seed for the TRNG-output PRNG (any nonzero constant). */
#define ELS_PRNG_SEED  0x2545F491u

/*
 * The PRNG/DRBG data-output register (ELS_PRNG_DATOUT) must return *fresh*
 * data on each read — that is what a TRNG does, and the NXP ELS entropy driver
 * reads it word-by-word to fill the kernel entropy pool.  Returning a constant
 * (the old behaviour) yields all-zero entropy, so anything seeded from it
 * (stack-pointer randomisation, CSPRNG) degenerates.  Back it with a small
 * xorshift32 PRNG: varying per read and reproducible per run (deterministic for
 * CI), not cryptographically strong — swap to qemu_guest_getrandom() if true
 * entropy is ever needed.
 */
static uint32_t els_prng_next(MCXNELSState *s)
{
    uint32_t x = s->rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng_state = x;
    return x;
}

static bool els_is_ro(hwaddr off)
{
    switch (off) {
    case ELS_STATUS:
    case ELS_ERR_STATUS:
    case ELS_VERSION:
    case ELS_PRNG_DATOUT:
    case ELS_CMDCRC:
    case ELS_DMA_FIN_ADDR:
        return true;
    default:
        /* Keystore status window ELS_KS0..ELS_KS19 is read-only. */
        return off >= ELS_KS0 && off <= ELS_KS19;
    }
}

static uint64_t mcxn_els_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNELSState *s = MCXN_ELS(opaque);
    uint32_t v = (off < MCXN_ELS_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case ELS_STATUS:
        /* Commands complete instantly: never busy, no error, PRNG ready. */
        return ELS_STATUS_IDLE;
    case ELS_ERR_STATUS:
        return 0;        /* no error pending */
    case ELS_VERSION:
        return ELS_VERSION_VALUE;
    case ELS_PRNG_DATOUT:
        return els_prng_next(s);   /* fresh random word per read */
    case ELS_INT_STATUS_CLR:
    case ELS_INT_STATUS_SET:
    case ELS_ERR_STATUS_CLR:
        return 0;        /* write-only */
    default:
        return v;
    }
}

static void mcxn_els_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    MCXNELSState *s = MCXN_ELS(opaque);

    if (off >= MCXN_ELS_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (els_is_ro(off)) {
        return;          /* read-only registers ignore writes */
    }
    /* Status-clear (WO) registers have no observable state in this model. */
    if (off == ELS_INT_STATUS_CLR || off == ELS_INT_STATUS_SET ||
        off == ELS_ERR_STATUS_CLR) {
        return;
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_els_ops = {
    .read = mcxn_els_read,
    .write = mcxn_els_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_els_reset(DeviceState *dev)
{
    MCXNELSState *s = MCXN_ELS(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->rng_state = ELS_PRNG_SEED;
}

static void mcxn_els_realize(DeviceState *dev, Error **errp)
{
    MCXNELSState *s = MCXN_ELS(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_els_ops, s,
                          TYPE_MCXN_ELS, MCXN_ELS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_els = {
    .name = TYPE_MCXN_ELS,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNELSState, MCXN_ELS_SIZE / 4),
        VMSTATE_UINT32(rng_state, MCXNELSState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_els_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_els_realize;
    device_class_set_legacy_reset(dc, mcxn_els_reset);
    dc->vmsd = &vmstate_mcxn_els;
}

static const TypeInfo mcxn_els_types[] = {
    {
        .name          = TYPE_MCXN_ELS,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNELSState),
        .class_init    = mcxn_els_class_init,
    },
};

DEFINE_TYPES(mcxn_els_types)
