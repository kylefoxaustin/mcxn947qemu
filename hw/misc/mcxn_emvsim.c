/*
 * NXP MCX N EMVSIM (EMV smartcard interface, UART-like) — bring-up model.
 *
 * One shared type for EMVSIM0/EMVSIM1.  The data path is simplified: TX_BUF
 * writes are accepted (the modelled "transmit") and TX_STATUS always reports
 * the transmitter empty/complete (TFE/TCF/TDTF) so a firmware TX poll completes
 * immediately.  RX_STATUS reports no received data and RX_BUF reads 0.  Both
 * status registers carry W1C flags.  Bit masks and offsets taken verbatim from
 * the MCXN947 CMSIS header (EMVSIM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_emvsim.h"
#include "migration/vmstate.h"

/* --- Register offsets ------------------------------------------------------ */
#define EMVSIM_VER_ID      0x00  /* RO */
#define EMVSIM_PARAM       0x04  /* RO */
#define EMVSIM_CLKCFG      0x08
#define EMVSIM_DIVISOR     0x0C
#define EMVSIM_CTRL        0x10
#define EMVSIM_INT_MASK    0x14
#define EMVSIM_RX_THD      0x18
#define EMVSIM_TX_THD      0x1C
#define EMVSIM_RX_STATUS   0x20  /* W1C status */
#define EMVSIM_TX_STATUS   0x24  /* W1C status */
#define EMVSIM_PCSR        0x28
#define EMVSIM_RX_BUF      0x2C  /* RO */
#define EMVSIM_TX_BUF      0x30  /* WO */
#define EMVSIM_TX_GETU     0x34
#define EMVSIM_CWT_VAL     0x38
#define EMVSIM_BWT_VAL     0x3C
#define EMVSIM_BGT_VAL     0x40
#define EMVSIM_GPCNT0_VAL  0x44
#define EMVSIM_GPCNT1_VAL  0x48

/* --- TX_STATUS bit masks (CMSIS) ------------------------------------------- */
#define TX_STATUS_TFE   0x00000008u  /* TX FIFO empty */
#define TX_STATUS_ETCF  0x00000010u  /* early transmit complete flag, W1C */
#define TX_STATUS_TCF   0x00000020u  /* transmit complete flag, W1C */
#define TX_STATUS_TFF   0x00000040u  /* TX FIFO full */
#define TX_STATUS_TDTF  0x00000080u  /* TX data transfer flag (threshold), W1C */
#define TX_STATUS_W1C   0x000003FFu  /* TNTE..GPCNT1_TO are all W1C */

/* --- RX_STATUS bit masks (CMSIS) ------------------------------------------- */
#define RX_STATUS_RX_DATA  0x00000010u  /* receiver has data */
#define RX_STATUS_W1C      0x00003FE1u  /* RFO + RDTF..FEF are W1C */

/*
 * VER_ID/PARAM are read-only identity registers.  Plausible MCX-class
 * constants; refine against the RM if a HAL depends on them.
 */
#define EMVSIM_VER_ID_VALUE  0x00000100u
#define EMVSIM_PARAM_VALUE   0x00000404u  /* RX/TX FIFO depth fields */

static uint64_t mcxn_emvsim_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(opaque);
    uint32_t r;

    switch (offset) {
    case EMVSIM_VER_ID:
        return EMVSIM_VER_ID_VALUE;
    case EMVSIM_PARAM:
        return EMVSIM_PARAM_VALUE;
    case EMVSIM_TX_STATUS:
        /*
         * Transmitter is always idle/ready in this model: FIFO empty, not
         * full, transmit complete and threshold met.  Preserve latched W1C
         * flags software set.
         */
        r = s->regs[EMVSIM_TX_STATUS / 4] & TX_STATUS_W1C;
        r &= ~TX_STATUS_TFF;
        r |= TX_STATUS_TFE | TX_STATUS_TCF | TX_STATUS_ETCF | TX_STATUS_TDTF;
        return r;
    case EMVSIM_RX_STATUS:
        /* No card data: report RX empty, keep any latched W1C flags. */
        r = s->regs[EMVSIM_RX_STATUS / 4] & RX_STATUS_W1C;
        r &= ~RX_STATUS_RX_DATA;
        return r;
    case EMVSIM_RX_BUF:
        return 0;  /* RO; no received data */
    case EMVSIM_TX_BUF:
        return 0;  /* write-only */
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_emvsim_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(opaque);

    switch (offset) {
    case EMVSIM_VER_ID:
    case EMVSIM_PARAM:
    case EMVSIM_RX_BUF:
        return;  /* read-only */
    case EMVSIM_TX_STATUS:
        s->regs[EMVSIM_TX_STATUS / 4] &= ~(value & TX_STATUS_W1C);
        return;
    case EMVSIM_RX_STATUS:
        s->regs[EMVSIM_RX_STATUS / 4] &= ~(value & RX_STATUS_W1C);
        return;
    case EMVSIM_TX_BUF:
        /* Accept and discard the transmitted byte (instantaneous TX). */
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_emvsim_ops = {
    .read = mcxn_emvsim_read,
    .write = mcxn_emvsim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_emvsim_reset(DeviceState *dev)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_emvsim_realize(DeviceState *dev, Error **errp)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_emvsim_ops, s,
                          TYPE_MCXN_EMVSIM, MCXN_EMVSIM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_emvsim = {
    .name = TYPE_MCXN_EMVSIM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNEMVSIMState, MCXN_EMVSIM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_emvsim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_emvsim_realize;
    device_class_set_legacy_reset(dc, mcxn_emvsim_reset);
    dc->vmsd = &vmstate_mcxn_emvsim;
}

static const TypeInfo mcxn_emvsim_types[] = {
    {
        .name          = TYPE_MCXN_EMVSIM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEMVSIMState),
        .class_init    = mcxn_emvsim_class_init,
    },
};

DEFINE_TYPES(mcxn_emvsim_types)
