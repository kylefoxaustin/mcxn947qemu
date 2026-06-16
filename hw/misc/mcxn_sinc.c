/*
 * NXP MCX N SINC (Sinc filter) — bring-up model.
 *
 * No real signal chain: per-channel conversions are reported "ready/done" so
 * firmware polling the global Status register (SR) or a channel status (CSR)
 * completes immediately, and the result-data registers (CRDATA) read 0.  The
 * normal/error/FIFO interrupt-status registers (NIS/EIS/FIFOIS) are modelled
 * W1C.  Bit masks and offsets taken verbatim from the MCXN947 CMSIS header
 * (SINC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_sinc.h"
#include "migration/vmstate.h"

/* --- Top-level register offsets -------------------------------------------- */
#define SINC_VERID      0x00  /* RO */
#define SINC_PARAMETER  0x04  /* RO */
#define SINC_MCR        0x08
#define SINC_NIE        0x0C
#define SINC_EIE        0x10
#define SINC_FIFOIE     0x14
#define SINC_NIS        0x18  /* W1C status */
#define SINC_EIS        0x1C  /* W1C status */
#define SINC_FIFOIS     0x20  /* W1C status */
#define SINC_SR         0x24  /* RO status */

/* --- Channel array: 5 channels, base 0x38, step 0x30 ----------------------- */
#define SINC_CH_BASE    0x38
#define SINC_CH_STEP    0x30
#define SINC_NUM_CH     5
#define SINC_CH_LAST    (SINC_CH_BASE + SINC_NUM_CH * SINC_CH_STEP) /* 0x128 */

#define SINC_CH_CRDATA  0x1C  /* RO result data, channel-relative */
#define SINC_CH_CSR     0x28  /* status, channel-relative */
#define SINC_CH_CDBGR   0x2C  /* RO debug, channel-relative */

/* --- SR (Status) bit masks (CMSIS) ----------------------------------------- */
/* CHRDY0..CHRDY4 : channel conversion-ready/done flags (bits 8..12). */
#define SR_CHRDY_ALL    0x00001F00u

/* --- CSR (channel status) bit masks (CMSIS) -------------------------------- */
#define CSR_PSRDY       0x00000080u  /* primary-stage ready */

/*
 * VERID/PARAMETER are read-only identity registers.  Plausible MCX-class
 * constants; refine against the RM if a HAL depends on them.
 */
#define SINC_VERID_VALUE      0x01000000u
#define SINC_PARAMETER_VALUE  0x00000005u  /* channel count */

static bool sinc_is_channel(hwaddr offset, hwaddr *rel)
{
    if (offset >= SINC_CH_BASE && offset < SINC_CH_LAST) {
        *rel = (offset - SINC_CH_BASE) % SINC_CH_STEP;
        return true;
    }
    return false;
}

static uint64_t mcxn_sinc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSINCState *s = MCXN_SINC(opaque);
    hwaddr rel;

    switch (offset) {
    case SINC_VERID:
        return SINC_VERID_VALUE;
    case SINC_PARAMETER:
        return SINC_PARAMETER_VALUE;
    case SINC_SR:
        /* Report every channel ready/done so conversion polls complete. */
        return SR_CHRDY_ALL;
    default:
        break;
    }

    if (sinc_is_channel(offset, &rel)) {
        switch (rel) {
        case SINC_CH_CRDATA:
        case SINC_CH_CDBGR:
            return 0;  /* RO; no real conversion result */
        case SINC_CH_CSR:
            /* Channel ready (primary stage) so per-channel polls complete. */
            return s->regs[offset / 4] | CSR_PSRDY;
        default:
            return s->regs[offset / 4];
        }
    }

    return s->regs[offset / 4];
}

static void mcxn_sinc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNSINCState *s = MCXN_SINC(opaque);
    hwaddr rel;

    switch (offset) {
    case SINC_VERID:
    case SINC_PARAMETER:
    case SINC_SR:
        return;  /* read-only */
    case SINC_NIS:
    case SINC_EIS:
    case SINC_FIFOIS:
        /* W1C interrupt status. */
        s->regs[offset / 4] &= ~(uint32_t)value;
        return;
    default:
        break;
    }

    if (sinc_is_channel(offset, &rel)) {
        if (rel == SINC_CH_CRDATA || rel == SINC_CH_CDBGR) {
            return;  /* read-only */
        }
    }

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_sinc_ops = {
    .read = mcxn_sinc_read,
    .write = mcxn_sinc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_sinc_reset(DeviceState *dev)
{
    MCXNSINCState *s = MCXN_SINC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_sinc_realize(DeviceState *dev, Error **errp)
{
    MCXNSINCState *s = MCXN_SINC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_sinc_ops, s,
                          TYPE_MCXN_SINC, MCXN_SINC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_sinc = {
    .name = TYPE_MCXN_SINC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSINCState, MCXN_SINC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_sinc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_sinc_realize;
    device_class_set_legacy_reset(dc, mcxn_sinc_reset);
    dc->vmsd = &vmstate_mcxn_sinc;
}

static const TypeInfo mcxn_sinc_types[] = {
    {
        .name          = TYPE_MCXN_SINC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSINCState),
        .class_init    = mcxn_sinc_class_init,
    },
};

DEFINE_TYPES(mcxn_sinc_types)
