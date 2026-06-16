/*
 * NXP MCX N DAC (12-bit DAC with output FIFO) — bring-up model.
 *
 * One shared type for DAC0/DAC1 (LPDAC) and DAC2 (HPDAC): all three expose an
 * identical register map.  The data path is simplified to "no real analog":
 * DATA writes are accepted (and remembered as the modelled output) and the FIFO
 * status (FSR) reports a ready/empty FIFO so firmware that polls for room never
 * spins.  Bit masks and offsets taken verbatim from the MCXN947 CMSIS header
 * (LPDAC_Type / HPDAC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_dac.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* --- Register offsets ------------------------------------------------------ */
#define DAC_VERID   0x00  /* RO */
#define DAC_PARAM   0x04  /* RO */
#define DAC_DATA    0x08  /* WO */
#define DAC_GCR     0x0C
#define DAC_FCR     0x10
#define DAC_FPR     0x14  /* RO */
#define DAC_FSR     0x18  /* W1C status bits */
#define DAC_IER     0x1C
#define DAC_DER     0x20
#define DAC_RCR     0x24
#define DAC_TCR     0x28  /* WO */
#define DAC_PCR     0x2C

/* --- FSR (FIFO Status) bit masks (CMSIS) ----------------------------------- */
#define FSR_FULL    0x00000001u
#define FSR_EMPTY   0x00000002u
#define FSR_WM      0x00000004u  /* watermark: room available */
#define FSR_SWBK    0x00000008u
#define FSR_OF      0x00000040u  /* overflow, W1C */
#define FSR_UF      0x00000080u  /* underflow, W1C */

#define FSR_W1C_MASK   (FSR_OF | FSR_UF)

/*
 * VERID/PARAM are read by some HALs to size the FIFO.  Values are plausible
 * MCX-class constants; refine against the RM if a HAL depends on them.
 */
#define DAC_VERID_VALUE  0x01000000u
#define DAC_PARAM_VALUE  0x00000004u  /* FIFOSZ field */

/*
 * IER (0x1C) interrupt-enable bits align one-to-one with the FSR flags
 * (FULL_IE@0, EMPTY_IE@1, WM_IE@2, ..., OF_IE@6, UF_IE@7).  The FIFO is always
 * drained, so EMPTY and WM (room available) are effectively asserted; plus any
 * latched overflow/underflow.  An interrupt is requested when an effective
 * flag and its enable are both set.
 */
static void mcxn_dac_update_irq(MCXNDACState *s)
{
    uint32_t fsr = (s->regs[DAC_FSR / 4] & FSR_W1C_MASK) | FSR_EMPTY | FSR_WM;
    uint32_t ier = s->regs[DAC_IER / 4];

    qemu_set_irq(s->irq, (fsr & ier) != 0);
}

static uint64_t mcxn_dac_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNDACState *s = MCXN_DAC(opaque);
    uint32_t r;

    switch (offset) {
    case DAC_VERID:
        return DAC_VERID_VALUE;
    case DAC_PARAM:
        return DAC_PARAM_VALUE;
    case DAC_DATA:
    case DAC_TCR:
        return 0;  /* write-only */
    case DAC_FPR:
        return 0;  /* FIFO pointers: empty FIFO */
    case DAC_FSR:
        /*
         * No real analog: the FIFO is always drained.  Report EMPTY and
         * watermark (room available) so writers are accepted and never full.
         * Preserve any latched (W1C) overflow/underflow flags software set.
         */
        r = s->regs[DAC_FSR / 4] & FSR_W1C_MASK;
        r |= FSR_EMPTY | FSR_WM;
        return r;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_dac_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNDACState *s = MCXN_DAC(opaque);

    switch (offset) {
    case DAC_VERID:
    case DAC_PARAM:
    case DAC_FPR:
        return;  /* read-only */
    case DAC_DATA:
        /* Accept the sample: this is the modelled DAC output. */
        s->data = value & 0xFFFFu;
        return;
    case DAC_FSR:
        /* W1C overflow/underflow; other bits are status (ignore writes). */
        s->regs[DAC_FSR / 4] &= ~(value & FSR_W1C_MASK);
        mcxn_dac_update_irq(s);
        return;
    case DAC_IER:
        s->regs[DAC_IER / 4] = value;
        mcxn_dac_update_irq(s);
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_dac_ops = {
    .read = mcxn_dac_read,
    .write = mcxn_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_dac_reset(DeviceState *dev)
{
    MCXNDACState *s = MCXN_DAC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->data = 0;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_dac_realize(DeviceState *dev, Error **errp)
{
    MCXNDACState *s = MCXN_DAC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_dac_ops, s,
                          TYPE_MCXN_DAC, MCXN_DAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_dac = {
    .name = TYPE_MCXN_DAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNDACState, MCXN_DAC_SIZE / 4),
        VMSTATE_UINT32(data, MCXNDACState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_dac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_dac_realize;
    device_class_set_legacy_reset(dc, mcxn_dac_reset);
    dc->vmsd = &vmstate_mcxn_dac;
}

static const TypeInfo mcxn_dac_types[] = {
    {
        .name          = TYPE_MCXN_DAC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNDACState),
        .class_init    = mcxn_dac_class_init,
    },
};

DEFINE_TYPES(mcxn_dac_types)
