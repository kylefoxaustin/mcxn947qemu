/*
 * NXP MCX N PINT (Pin Interrupt and Pattern Match) — functional model.
 *
 * 8 pin-interrupt channels sharing one NVIC line (PINT0_IRQn = 47).  The IENR
 * (rising) and IENF (falling) edge enables have write-only set/clear aliases:
 * writing a 1 to SIENR/SIENF sets the matching IENR/IENF bit, writing a 1 to
 * CIENR/CIENF clears it.  RISE/FALL are write-1-to-clear edge-detect flags and
 * IST is the write-1-to-clear status; ISEL and the pattern-match registers are
 * plain storage.  Offsets from the MCXN947 CMSIS header (PINT_Type).
 *
 * A pin edge has no source in emulation, so the 8 channel input levels are
 * OPERATOR-DRIVEN via the "pin-input" QOM property (like the CMP comparator
 * output).  A change edge-detects each channel: a rising edge on an
 * IENR-enabled channel sets RISE+IST, a falling edge on an IENF-enabled
 * channel sets FALL+IST, the IRQ tracks IST, and for channels 0..3 the edge
 * also pulses the eDMA request (CMSIS PINT INT0..3 = sources 3..6) — the
 * pin-paced-DMA path.  Only edge mode (ISEL[ch]=0) is modelled; the
 * level-sensitive mode is a stated boundary.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_pint.h"
#include "hw/core/irq.h"
#include "qapi/visitor.h"
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

#define PINT_PIN_MASK  0xFFu   /* 8 channels, bits [7:0] of each register */

/* IRQ tracks any pending IST bit. */
static void mcxn_pint_update_irq(MCXNPINTState *s)
{
    qemu_set_irq(s->irq, (s->regs[PINT_IST / 4] & PINT_PIN_MASK) != 0);
}

/*
 * The operator drove the 8 channel input levels.  Edge-detect each channel
 * against the previous level: a rising edge on an IENR-enabled channel latches
 * RISE + IST, a falling edge on an IENF-enabled channel latches FALL + IST, and
 * for channels 0..3 the edge also pulses the eDMA request (a one-shot PULSE,
 * PINT INT0..3 = sources 3..6).  Only edge mode (ISEL[ch]=0) is modelled.
 */
static void mcxn_pint_drive_input(MCXNPINTState *s, uint8_t level)
{
    uint8_t isel = s->regs[PINT_ISEL / 4] & PINT_PIN_MASK;
    uint8_t ienr = s->regs[PINT_IENR / 4] & PINT_PIN_MASK;
    uint8_t ienf = s->regs[PINT_IENF / 4] & PINT_PIN_MASK;
    uint8_t old = s->pin_level;
    int ch;

    for (ch = 0; ch < MCXN_PINT_CHANNELS; ch++) {
        uint8_t m = 1u << ch;
        bool was = (old & m) != 0;
        bool now = (level & m) != 0;
        bool fired = false;

        if (isel & m) {
            /* level-sensitive mode: not modelled */
            continue;
        }
        if (now && !was && (ienr & m)) {          /* rising edge, enabled  */
            s->regs[PINT_RISE / 4] |= m;
            s->regs[PINT_IST / 4]  |= m;
            fired = true;
        } else if (!now && was && (ienf & m)) {   /* falling edge, enabled */
            s->regs[PINT_FALL / 4] |= m;
            s->regs[PINT_IST / 4]  |= m;
            fired = true;
        }
        if (fired && ch < MCXN_PINT_DMA_LINES) {
            qemu_irq_pulse(s->dma_req[ch]);
        }
    }
    s->pin_level = level;
    mcxn_pint_update_irq(s);
}

static void mcxn_pint_get_input(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint8_t val = MCXN_PINT(obj)->pin_level;

    visit_type_uint8(v, name, &val, errp);
}

static void mcxn_pint_set_input(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint8_t val;

    if (!visit_type_uint8(v, name, &val, errp)) {
        return;
    }
    mcxn_pint_drive_input(MCXN_PINT(obj), val);
}

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
    default:
        /*
         * RISE/FALL/IST are real edge/status state now; everything else is
         * plain storage.
         */
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
        /* Write-1-to-clear edge-detect flags. */
        s->regs[off >> 2] &= ~v;
        break;
    case PINT_IST:
        /*
         * Write-1-to-clear status; clearing the last pending bit drops
         * the IRQ.
         */
        s->regs[PINT_IST / 4] &= ~v;
        mcxn_pint_update_irq(s);
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
    s->pin_level = 0;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_pint_init(Object *obj)
{
    /*
     * Operator-driven pin input: the 8 channel levels the selected pins
     * would resolve to.  qom-set /machine/soc/pint0 pin-input 1  drives
     * channel 0 high.
     */
    object_property_add(obj, "pin-input", "uint8",
                        mcxn_pint_get_input, mcxn_pint_set_input, NULL, NULL);
}

static void mcxn_pint_realize(DeviceState *dev, Error **errp)
{
    MCXNPINTState *s = MCXN_PINT(dev);
    int i;

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_pint_ops, s,
                          TYPE_MCXN_PINT, MCXN_PINT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    /* 0: PINT0_IRQn = 47 */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    for (i = 0; i < MCXN_PINT_DMA_LINES; i++) {
        /* 1..4: INT0..3 DMA */
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req[i]);
    }
}

static const VMStateDescription vmstate_mcxn_pint = {
    .name = TYPE_MCXN_PINT,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPINTState, MCXN_PINT_SIZE / 4),
        VMSTATE_UINT8(pin_level, MCXNPINTState),
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
        .instance_init = mcxn_pint_init,
        .class_init    = mcxn_pint_class_init,
    },
};

DEFINE_TYPES(mcxn_pint_types)
