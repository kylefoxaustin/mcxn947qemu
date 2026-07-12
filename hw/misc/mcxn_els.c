/*
 * NXP MCX N ELS (EdgeLock Secure subsystem, S50) — honest crypto model.
 *
 * The ELS is a hardware crypto engine.  We do not implement AES, SHA, HMAC,
 * ECDSA or key derivation, and we are never going to fake them: a crypto engine
 * that reports success without computing is the most dangerous silent-wrong a
 * machine model can contain.
 *
 * The trap, and why the previous model fell into it: EVERY dangerous ELS command
 * writes its result by DMA into a guest buffer (ELS_DMA_RES0), not into a
 * register.  The old model latched the command, computed nothing, left the result
 * buffer UNTOUCHED, and then reported ELS_STATUS as "not busy, no error" with
 * ELS_ERR_STATUS hardwired to 0.  So firmware ran an ECDSA sign, a SHA-256, an
 * AES encrypt or a key-derivation, saw a clean completion, and read uninitialised
 * memory as its signature / digest / ciphertext / session key.  An ECDSA VERIFY
 * would "succeed" against garbage.  Nothing — not the guest, not the host log —
 * knew.  (Class identified by rt1180emulator, who found the identical bug in his
 * EdgeLock message unit: "if your uncomputed-flag is gated on reply SHAPE rather
 * than command SEMANTICS, the commands that write their result to a buffer are
 * the ones your heuristic will miss, and they are the ones that matter.")
 *
 * So this model splits the command set by what it can honestly do:
 *
 *   HONOURED   RND_REQ  — we have real entropy, so we DMA real random bytes into
 *                         the result buffer.  This is a genuine data path.
 *              DTRNG config / DRBG test / key delete — no result data to fake.
 *
 *   FAULTED    every actual cryptographic operation (cipher, AEAD, hash, HMAC,
 *              CMAC, ECDSA sign/verify, ECDH, key gen/in/out/prov, CKDF/HKDF,
 *              TLS).  The engine reports the failure through its OWN documented
 *              error channel: ELS_STATUS[ELS_ERR] + ELS_ERR_STATUS[OPN_ERR].
 *
 * The channel matters.  BUSY still clears, so the driver's "wait for done" loop
 * always terminates — the guest is never hung, it is told.  mcuxClEls checks
 * ELS_STATUS[ELS_ERR] after the wait and returns an error to its caller, which is
 * exactly what silicon would do for an operation it could not perform.  Faulting
 * through the completion path instead would hang the driver rather than inform it
 * (fleet finding).
 *
 * The PRNG behind ELS_PRNG_DATOUT and RND_REQ is a deterministic xorshift32: it
 * is real, varying entropy — enough that anything seeded from it (Zephyr's stack
 * randomisation, a CSPRNG) does not degenerate — but it is NOT cryptographically
 * strong, and firmware must not be relied on to notice.  Swap to
 * qemu_guest_getrandom() if that ever matters.
 *
 * Offsets/bits from the MCXN947 CMSIS header (S50_Type); command IDs from the
 * MCUXpresso els_pkc driver (mcuxClEls_Crc.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_els.h"
#include "system/dma.h"
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
#define ELS_DMA_RES0        0x38  /* where a command's result is written */
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

/* ELS_STATUS field masks (CMSIS S50_ELS_STATUS_*). */
#define ELS_STATUS_ELS_BUSY     (1u << 0)
#define ELS_STATUS_ELS_IRQ      (1u << 1)
#define ELS_STATUS_ELS_ERR      (1u << 2)
#define ELS_STATUS_PRNG_RDY     (1u << 3)
#define ELS_STATUS_DTRNG_BUSY   (1u << 10)
#define ELS_STATUS_DRBG_ENT_MAX 0x300u   /* DRBG entropy level = max */

/* ELS_CTRL fields (CMSIS S50_ELS_CTRL_*). */
#define ELS_CTRL_EN     (1u << 0)
#define ELS_CTRL_START  (1u << 1)
#define ELS_CTRL_RESET  (1u << 2)
#define ELS_CTRL_CMD(v) (((v) >> 3) & 0x1F)

/* ELS_ERR_STATUS bits (CMSIS S50_ELS_ERR_STATUS_*). */
#define ELS_ERR_OPN     (1u << 1)   /* operation error: could not be performed */

/* ELS command IDs (MCUXpresso mcuxClEls_Crc.h, ELS_CTRL[ELS_CMD]). */
#define ELS_CMD_CIPHER          0
#define ELS_CMD_AUTH_CIPHER     1
#define ELS_CMD_CHAL_RESP_GEN   3
#define ELS_CMD_ECSIGN          4
#define ELS_CMD_ECVFY           5
#define ELS_CMD_ECKXH           6
#define ELS_CMD_KEYGEN          8
#define ELS_CMD_KEYIN           9
#define ELS_CMD_KEYOUT          10
#define ELS_CMD_KDELETE         11
#define ELS_CMD_KEYPROV         12
#define ELS_CMD_CKDF            16
#define ELS_CMD_HKDF            17
#define ELS_CMD_TLS             18
#define ELS_CMD_HASH            20
#define ELS_CMD_HMAC            21
#define ELS_CMD_CMAC            22
#define ELS_CMD_RND_REQ         24
#define ELS_CMD_DRBG_TEST       25
#define ELS_CMD_DTRNG_CFG_LOAD  28

/* Version register value (X.Y1.Y2.Z = 1.0.0.0). Unconfirmed against RM. */
#define ELS_VERSION_VALUE  0x00001000u

/* Seed for the entropy PRNG (any nonzero constant). */
#define ELS_PRNG_SEED  0x2545F491u

/* Largest RND_REQ we will service in one command. */
#define ELS_RND_MAX  4096

/*
 * Real, varying entropy — not cryptographically strong.  A constant here yields
 * all-zero entropy and anything seeded from it (stack-pointer randomisation, a
 * CSPRNG) degenerates silently.
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

static const char *els_cmd_name(uint32_t cmd)
{
    switch (cmd) {
    case ELS_CMD_CIPHER:        return "CIPHER (AES)";
    case ELS_CMD_AUTH_CIPHER:   return "AUTH_CIPHER (AEAD)";
    case ELS_CMD_CHAL_RESP_GEN: return "CHAL_RESP_GEN";
    case ELS_CMD_ECSIGN:        return "ECSIGN (ECDSA sign)";
    case ELS_CMD_ECVFY:         return "ECVFY (ECDSA verify)";
    case ELS_CMD_ECKXH:         return "ECKXH (ECDH)";
    case ELS_CMD_KEYGEN:        return "KEYGEN";
    case ELS_CMD_KEYIN:         return "KEYIN";
    case ELS_CMD_KEYOUT:        return "KEYOUT";
    case ELS_CMD_KEYPROV:       return "KEYPROV";
    case ELS_CMD_CKDF:          return "CKDF";
    case ELS_CMD_HKDF:          return "HKDF";
    case ELS_CMD_TLS:           return "TLS";
    case ELS_CMD_HASH:          return "HASH";
    case ELS_CMD_HMAC:          return "HMAC";
    case ELS_CMD_CMAC:          return "CMAC";
    default:                    return "unknown";
    }
}

/* RND_REQ: the one command we can honestly satisfy — DMA real random bytes. */
static void els_do_rnd_req(MCXNELSState *s)
{
    uint32_t addr = s->regs[ELS_DMA_RES0 / 4];
    uint32_t len  = s->regs[ELS_DMA_RES0_LEN / 4];
    uint8_t buf[ELS_RND_MAX];

    if (!len || len > ELS_RND_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "mcxn-els: RND_REQ of %u bytes is outside the modelled "
                      "range; failing the command rather than returning weak or "
                      "no entropy\n", len);
        s->err_status |= ELS_ERR_OPN;
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = els_prng_next(s) & 0xFF;
    }
    dma_memory_write(&address_space_memory, addr, buf, len,
                     MEMTXATTRS_UNSPECIFIED);
}

/*
 * ELS_CTRL[ELS_START]: run the staged command.
 *
 * Anything that produces a cryptographic RESULT is faulted, never faked.  The
 * result would land in the guest's ELS_DMA_RES0 buffer, so "succeeding" without
 * writing it hands firmware uninitialised memory as a signature, digest, key or
 * ciphertext — and firmware has no way to tell.
 */
static void els_start_command(MCXNELSState *s)
{
    uint32_t cmd = ELS_CTRL_CMD(s->regs[ELS_CTRL / 4]);

    switch (cmd) {
    case ELS_CMD_RND_REQ:
        els_do_rnd_req(s);
        return;

    case ELS_CMD_DRBG_TEST:
    case ELS_CMD_DTRNG_CFG_LOAD:
    case ELS_CMD_KDELETE:
        /* Configuration / teardown: no result data, nothing to fabricate. */
        return;

    default:
        /*
         * A real cryptographic operation we do not implement.  Report it through
         * the engine's own error channel so mcuxClEls returns a failure to its
         * caller.  BUSY still clears, so the driver's wait loop terminates: the
         * guest is informed, not hung.
         */
        s->err_status |= ELS_ERR_OPN;
        qemu_log_mask(LOG_UNIMP,
                      "mcxn-els: command %u (%s) is NOT COMPUTED — failing it via "
                      "ELS_STATUS[ELS_ERR]/ELS_ERR_STATUS[OPN_ERR] rather than "
                      "reporting success over an untouched result buffer.  The "
                      "ELS crypto engine is not modelled; firmware must treat this "
                      "operation as failed, not trust the DMA_RES0 contents.\n",
                      cmd, els_cmd_name(cmd));
        return;
    }
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
    uint32_t st;

    switch (off) {
    case ELS_STATUS:
        /*
         * Commands retire instantly, so BUSY is never set and no wait loop can
         * hang.  ELS_ERR, however, is REAL: it is set whenever the engine was
         * asked for a cryptographic result it cannot produce.
         */
        st = ELS_STATUS_PRNG_RDY | ELS_STATUS_DRBG_ENT_MAX;
        if (s->err_status) {
            st |= ELS_STATUS_ELS_ERR;
        }
        return st;
    case ELS_ERR_STATUS:
        return s->err_status;
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
    uint32_t val = value;

    if (off >= MCXN_ELS_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (els_is_ro(off)) {
        return;          /* read-only registers ignore writes */
    }

    switch (off) {
    case ELS_ERR_STATUS_CLR:
        /* W1C: firmware clears the error and may retry. */
        s->err_status &= ~val;
        return;
    case ELS_INT_STATUS_CLR:
    case ELS_INT_STATUS_SET:
        return;          /* no observable interrupt state in this model */
    case ELS_CTRL:
        s->regs[ELS_CTRL / 4] = val;
        if (val & ELS_CTRL_RESET) {
            s->err_status = 0;
            return;
        }
        if ((val & ELS_CTRL_EN) && (val & ELS_CTRL_START)) {
            els_start_command(s);
        }
        return;
    default:
        s->regs[off >> 2] = val;
        return;
    }
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
    s->err_status = 0;
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
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNELSState, MCXN_ELS_SIZE / 4),
        VMSTATE_UINT32(rng_state, MCXNELSState),
        VMSTATE_UINT32(err_status, MCXNELSState),
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
