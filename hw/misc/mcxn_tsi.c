/*
 * NXP MCX N TSI (Touch Sensing Input) — bring-up model.  See header.
 *
 * Firmware enables the block (GENCS[TSIEN]), starts a scan (software trigger
 * GENCS[SWTS], or periodic via GENCS[STM]) and polls the end-of-scan flag
 * DATA[EOSF] before reading the conversion counter DATA[TSICNT].  This model
 * completes a scan the instant a software trigger is observed: DATA[EOSF] sets
 * and DATA[TSICNT] reads a plausible count.  EOSF and the overrun/out-of-range
 * flags are write-1-to-clear, so the scan-complete handshake finishes.  All
 * other registers are permissively backed.  Offsets/bits from the MCXN947
 * CMSIS header (TSI_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_tsi.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS TSI_Type). */
#define R_CONFIG    0x00   /* CONFIG / CONFIG_MUTUAL (union) */
#define R_TSHD      0x04
#define R_GENCS     0x08
#define R_MUL       0x0C
#define R_SINC      0x10
#define R_SSC0      0x14
#define R_SSC1      0x18
#define R_SSC2      0x1C
#define R_BASELINE  0x20
#define R_CHMERGE   0x24
#define R_SHIELD    0x28
#define R_DATA      0x100
#define R_MISC      0x108
#define R_TRIG      0x10C

/* GENCS bits. */
#define GENCS_STM       (1u << 3)    /* scan trigger mode (periodic) */
#define GENCS_TSIEN     (1u << 5)    /* module enable */
#define GENCS_SWTS      (1u << 7)    /* software trigger, self-clearing */

/* DATA bits (the flags are write-1-to-clear). */
#define DATA_TSICNT_MASK 0xFFFFu
#define DATA_EOSF        (1u << 27)  /* end-of-scan flag */
#define DATA_OVERRUNF    (1u << 29)
#define DATA_OUTRGF      (1u << 30)
#define DATA_W1C_MASK    (DATA_EOSF | DATA_OVERRUNF | DATA_OUTRGF)

/* A plausible touch counter reading. */
#define TSI_COUNT_SAMPLE 0x0100u

static void mcxn_tsi_update_irq(MCXNTSIState *s)
{
    /* Assert while the module is enabled and the end-of-scan flag is set. */
    bool active = (s->regs[R_GENCS / 4] & GENCS_TSIEN) &&
                  (s->regs[R_DATA / 4] & DATA_EOSF);
    qemu_set_irq(s->irq, active);
}

/* Complete a scan: latch a count and raise the end-of-scan flag. */
static void mcxn_tsi_do_scan(MCXNTSIState *s)
{
    if (!(s->regs[R_GENCS / 4] & GENCS_TSIEN)) {
        return;
    }
    s->regs[R_DATA / 4] = (s->regs[R_DATA / 4] & ~DATA_TSICNT_MASK) |
                          TSI_COUNT_SAMPLE | DATA_EOSF;
    mcxn_tsi_update_irq(s);
}

static uint64_t mcxn_tsi_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNTSIState *s = MCXN_TSI(opaque);

    switch (offset) {
    case R_GENCS:
        /* SWTS reads back 0 (the trigger is momentary). */
        return s->regs[R_GENCS / 4] & ~GENCS_SWTS;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_tsi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNTSIState *s = MCXN_TSI(opaque);
    uint32_t v = value;

    switch (offset) {
    case R_GENCS:
        /* SWTS is a self-clearing software trigger. */
        s->regs[R_GENCS / 4] = v & ~GENCS_SWTS;
        if ((v & GENCS_SWTS) || (v & GENCS_STM)) {
            mcxn_tsi_do_scan(s);
        } else {
            mcxn_tsi_update_irq(s);
        }
        return;
    case R_DATA: {
        /* Write-1-to-clear the flag bits; the count field is read-only. */
        uint32_t cur = s->regs[R_DATA / 4];
        cur &= ~(v & DATA_W1C_MASK);
        s->regs[R_DATA / 4] = cur;
        mcxn_tsi_update_irq(s);
        return;
    }
    default:
        s->regs[offset / 4] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_tsi_ops = {
    .read = mcxn_tsi_read,
    .write = mcxn_tsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_tsi_reset(DeviceState *dev)
{
    MCXNTSIState *s = MCXN_TSI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void mcxn_tsi_realize(DeviceState *dev, Error **errp)
{
    MCXNTSIState *s = MCXN_TSI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_tsi_ops, s,
                          TYPE_MCXN_TSI, MCXN_TSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_tsi = {
    .name = TYPE_MCXN_TSI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNTSIState, MCXN_TSI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_tsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_tsi_realize;
    device_class_set_legacy_reset(dc, mcxn_tsi_reset);
    dc->vmsd = &vmstate_mcxn_tsi;
}

static const TypeInfo mcxn_tsi_types[] = {
    {
        .name          = TYPE_MCXN_TSI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNTSIState),
        .class_init    = mcxn_tsi_class_init,
    },
};

DEFINE_TYPES(mcxn_tsi_types)
