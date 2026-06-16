/*
 * NXP MCX N SCT (SCTimer/PWM) — bring-up model.  See header for the modelled
 * semantics.  Single instance ("mcxn-sct") for SCT0.
 *
 * Register layout from the MCXN947 CMSIS header (SCT_Type).  Offsets/bit masks
 * are taken verbatim from that header; reset values default to zero (the RM
 * gives 0x00000000 reset for the registers exercised at bring-up).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_sct.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS SCT_Type). */
#define SCT_CONFIG   0x00
#define SCT_CTRL     0x04
#define SCT_COUNT    0x40
#define SCT_EVFLAG   0xF4    /* Event Flag (W1C) */
#define SCT_CONFLAG  0xFC    /* Conflict Flag (W1C) */

/* CTRL self-clearing counter-clear bits (low and high 16-bit counter halves). */
#define SCT_CTRL_CLRCTR_L_MASK   0x00000008u
#define SCT_CTRL_CLRCTR_H_MASK   0x00080000u

static inline uint32_t sct_ld32(MCXNSCTState *s, hwaddr off)
{
    return s->regs[off] |
           ((uint32_t)s->regs[off + 1] << 8) |
           ((uint32_t)s->regs[off + 2] << 16) |
           ((uint32_t)s->regs[off + 3] << 24);
}

static inline void sct_st32(MCXNSCTState *s, hwaddr off, uint32_t v)
{
    s->regs[off] = v & 0xff;
    s->regs[off + 1] = (v >> 8) & 0xff;
    s->regs[off + 2] = (v >> 16) & 0xff;
    s->regs[off + 3] = (v >> 24) & 0xff;
}

static uint64_t mcxn_sct_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSCTState *s = MCXN_SCT(opaque);
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_SCT_SIZE) {
            val |= (uint64_t)s->regs[offset + i] << (8 * i);
        }
    }
    return val;
}

static void mcxn_sct_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNSCTState *s = MCXN_SCT(opaque);
    unsigned i;

    /* CTRL: STOP/HALT and the rest of the configuration reflect back, but the
     * CLRCTR_L/CLRCTR_H bits are self-clearing: writing 1 zeroes the matching
     * COUNT half and the bit itself never reads back set. */
    if (size == 4 && offset == SCT_CTRL) {
        uint32_t v = value & 0xffffffffu;
        uint32_t count = sct_ld32(s, SCT_COUNT);

        if (v & SCT_CTRL_CLRCTR_L_MASK) {
            count &= 0xffff0000u;
        }
        if (v & SCT_CTRL_CLRCTR_H_MASK) {
            count &= 0x0000ffffu;
        }
        sct_st32(s, SCT_COUNT, count);
        v &= ~(SCT_CTRL_CLRCTR_L_MASK | SCT_CTRL_CLRCTR_H_MASK);
        sct_st32(s, SCT_CTRL, v);
        return;
    }

    /* EVFLAG / CONFLAG: write-1-to-clear. */
    if (size == 4 && (offset == SCT_EVFLAG || offset == SCT_CONFLAG)) {
        uint32_t cur = sct_ld32(s, offset);
        cur &= ~(uint32_t)value;
        sct_st32(s, offset, cur);
        return;
    }

    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_SCT_SIZE) {
            s->regs[offset + i] = (value >> (8 * i)) & 0xff;
        }
    }
}

static const MemoryRegionOps mcxn_sct_ops = {
    .read = mcxn_sct_read,
    .write = mcxn_sct_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_sct_reset(DeviceState *dev)
{
    MCXNSCTState *s = MCXN_SCT(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_sct_realize(DeviceState *dev, Error **errp)
{
    MCXNSCTState *s = MCXN_SCT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_sct_ops, s,
                          TYPE_MCXN_SCT, MCXN_SCT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_sct = {
    .name = TYPE_MCXN_SCT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MCXNSCTState, MCXN_SCT_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_sct_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_sct_realize;
    device_class_set_legacy_reset(dc, mcxn_sct_reset);
    dc->vmsd = &vmstate_mcxn_sct;
}

static const TypeInfo mcxn_sct_types[] = {
    {
        .name          = TYPE_MCXN_SCT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSCTState),
        .class_init    = mcxn_sct_class_init,
    },
};

DEFINE_TYPES(mcxn_sct_types)
