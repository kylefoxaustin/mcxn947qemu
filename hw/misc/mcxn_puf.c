/*
 * NXP MCX N PUF (Physically Unclonable Function) - bring-up model.
 *
 * Faithful register file, no real PUF.  Firmware issues commands (enroll,
 * start, reconstruct, get-key, ...) via the CR register and then polls SR for
 * the BUSY bit to clear, checking OK / ERROR.  Here the model reports the
 * engine permanently idle and the last operation successful: SR.BUSY reads 0,
 * SR.OK reads 1, SR.ERROR reads 0, and SRAM_STATUS.READY reads ready, so the
 * enroll/start/get-key/SRAM-init poll loops all complete.  The AR (Allow)
 * register is read-only and reports every operation permitted.  The *_INT_*
 * clear/set registers are write-only and ignored; read-only ID/version and
 * data-output registers return constants/backing store.  Offsets/bits from the
 * MCXN947 CMSIS header (PUF_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_puf.h"
#include "migration/vmstate.h"

/* Register offsets (PUF_Type). */
#define PUF_CR                  0x00
#define PUF_ORR                 0x04  /* RO: operation result */
#define PUF_SR                  0x08  /* status */
#define PUF_AR                  0x0C  /* RO: allow */
#define PUF_IER                 0x10
#define PUF_IMR                 0x14
#define PUF_ISR                 0x18
#define PUF_DATA_DEST           0x20
#define PUF_DATA_SRC            0x24
#define PUF_DIR                 0xA0  /* WO: data input */
#define PUF_DOR                 0xA8  /* RO: data output */
#define PUF_MISC                0xC0
#define PUF_IF_SR               0xD0  /* interface status */
#define PUF_PSR                 0xDC  /* RO: PUF score */
#define PUF_HW_RUC0             0xE0  /* RO */
#define PUF_HW_RUC1             0xE4  /* RO */
#define PUF_HW_INFO             0xF4  /* RO */
#define PUF_HW_ID               0xF8  /* RO */
#define PUF_HW_VER              0xFC  /* RO */
#define PUF_CONFIG              0x100
#define PUF_SEC_LOCK            0x104
#define PUF_APP_CTX_MASK        0x108
#define PUF_SRAM_CFG            0x300
#define PUF_SRAM_STATUS         0x304 /* RO */
#define PUF_SRAM_INT_CLR_ENABLE 0x3D8 /* WO */
#define PUF_SRAM_INT_SET_ENABLE 0x3DC /* WO */
#define PUF_SRAM_INT_STATUS     0x3E0 /* RO */
#define PUF_SRAM_INT_ENABLE     0x3E4 /* RO */
#define PUF_SRAM_INT_CLR_STATUS 0x3E8 /* WO */
#define PUF_SRAM_INT_SET_STATUS 0x3EC /* WO */

/* SR field masks. */
#define PUF_SR_BUSY    (1u << 0)
#define PUF_SR_OK      (1u << 1)
#define PUF_SR_ERROR   (1u << 2)

/* Idle/last-operation-OK status: not busy, OK set, no error. */
#define PUF_SR_IDLE_OK  PUF_SR_OK

/* SRAM_STATUS.READY */
#define PUF_SRAM_STATUS_READY  (1u << 0)

/* AR (Allow): permit every PUF operation. */
#define PUF_AR_ALLOW_ALL  0xC00083EEu

/* Read-only ID/version constants.  Unconfirmed against RM. */
#define PUF_HW_ID_VALUE   0x00000000u
#define PUF_HW_VER_VALUE  0x00010000u   /* major 1, minor 0, rev 0 */
#define PUF_HW_INFO_VALUE 0x00000000u

static bool puf_is_ro(hwaddr off)
{
    switch (off) {
    case PUF_ORR:
    case PUF_AR:
    case PUF_DOR:
    case PUF_PSR:
    case PUF_HW_RUC0:
    case PUF_HW_RUC1:
    case PUF_HW_INFO:
    case PUF_HW_ID:
    case PUF_HW_VER:
    case PUF_SRAM_STATUS:
    case PUF_SRAM_INT_STATUS:
    case PUF_SRAM_INT_ENABLE:
        return true;
    default:
        return false;
    }
}

static bool puf_is_wo(hwaddr off)
{
    switch (off) {
    case PUF_DIR:
    case PUF_SRAM_INT_CLR_ENABLE:
    case PUF_SRAM_INT_SET_ENABLE:
    case PUF_SRAM_INT_CLR_STATUS:
    case PUF_SRAM_INT_SET_STATUS:
        return true;
    default:
        return false;
    }
}

static uint64_t mcxn_puf_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNPUFState *s = MCXN_PUF(opaque);
    uint32_t v = (off < MCXN_PUF_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case PUF_SR:
        /* Commands complete instantly and succeed. */
        return PUF_SR_IDLE_OK;
    case PUF_AR:
        return PUF_AR_ALLOW_ALL;
    case PUF_SRAM_STATUS:
        return PUF_SRAM_STATUS_READY;
    case PUF_IF_SR:
        return 0;        /* no interface error */
    case PUF_HW_ID:
        return PUF_HW_ID_VALUE;
    case PUF_HW_VER:
        return PUF_HW_VER_VALUE;
    case PUF_HW_INFO:
        return PUF_HW_INFO_VALUE;
    default:
        if (puf_is_wo(off)) {
            return 0;
        }
        return v;
    }
}

static void mcxn_puf_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    MCXNPUFState *s = MCXN_PUF(opaque);

    if (off >= MCXN_PUF_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (puf_is_ro(off)) {
        return;          /* read-only registers ignore writes */
    }
    if (puf_is_wo(off)) {
        return;          /* write-only side effects: no observable state */
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_puf_ops = {
    .read = mcxn_puf_read,
    .write = mcxn_puf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_puf_reset(DeviceState *dev)
{
    MCXNPUFState *s = MCXN_PUF(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_puf_realize(DeviceState *dev, Error **errp)
{
    MCXNPUFState *s = MCXN_PUF(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_puf_ops, s,
                          TYPE_MCXN_PUF, MCXN_PUF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_puf = {
    .name = TYPE_MCXN_PUF,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPUFState, MCXN_PUF_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_puf_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_puf_realize;
    device_class_set_legacy_reset(dc, mcxn_puf_reset);
    dc->vmsd = &vmstate_mcxn_puf;
}

static const TypeInfo mcxn_puf_types[] = {
    {
        .name          = TYPE_MCXN_PUF,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPUFState),
        .class_init    = mcxn_puf_class_init,
    },
};

DEFINE_TYPES(mcxn_puf_types)
