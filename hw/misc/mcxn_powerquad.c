/*
 * NXP MCX N POWERQUAD (DSP math coprocessor) - bring-up model.  See header for
 * design notes.  Offsets and bits from the MCXN947 CMSIS header
 * (POWERQUAD_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_powerquad.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS POWERQUAD_Type). */
#define R_CONTROL   0x100  /* RW: opcode/machine launch; INST_BUSY in bit 31 */
#define R_LENGTH    0x104
#define R_CPPRE     0x108
#define R_MISC      0x10C
#define R_CURSORY   0x110
#define R_ERRSTAT   0x18C  /* error status (W1C) */
#define R_INTREN    0x190
#define R_EVENTEN   0x194
#define R_INTRSTAT  0x198  /* interrupt status (W1C) */

#define CONTROL_INST_BUSY  (1u << 31)  /* reads 0 (idle) in this model */
#define INTRSTAT_INTR_STAT (1u << 0)   /* completion interrupt status (W1C) */
#define ERRSTAT_MASK       0x1Fu       /* OVERFLOW/NAN/FIXEDOVERFLOW/UFLOW/BERR */

static uint64_t mcxn_powerquad_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(opaque);
    uint32_t v = (off < MCXN_POWERQUAD_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case R_CONTROL:
        /*
         * Every instruction retires instantly: never report INST_BUSY so a
         * launch-then-poll loop completes on the first read.
         */
        return v & ~CONTROL_INST_BUSY;
    default:
        return v;
    }
}

static void mcxn_powerquad_write(void *opaque, hwaddr off,
                                 uint64_t value, unsigned size)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(opaque);
    uint32_t v = value;

    if (off >= MCXN_POWERQUAD_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_CONTROL:
        /*
         * Reflect the launch back without the busy bit: the instruction is
         * already "done" the moment it is written.
         */
        s->regs[off >> 2] = v & ~CONTROL_INST_BUSY;
        return;
    case R_ERRSTAT:
        /* Error flags are write-1-to-clear; no errors are ever generated. */
        s->regs[off >> 2] &= ~(v & ERRSTAT_MASK);
        return;
    case R_INTRSTAT:
        /* Completion interrupt status is write-1-to-clear. */
        s->regs[off >> 2] &= ~(v & INTRSTAT_INTR_STAT);
        qemu_set_irq(s->irq, 0);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_powerquad_ops = {
    .read = mcxn_powerquad_read,
    .write = mcxn_powerquad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_powerquad_reset(DeviceState *dev)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void mcxn_powerquad_realize(DeviceState *dev, Error **errp)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_powerquad_ops, s,
                          TYPE_MCXN_POWERQUAD, MCXN_POWERQUAD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_powerquad = {
    .name = TYPE_MCXN_POWERQUAD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPowerQuadState, MCXN_POWERQUAD_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_powerquad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_powerquad_realize;
    device_class_set_legacy_reset(dc, mcxn_powerquad_reset);
    dc->vmsd = &vmstate_mcxn_powerquad;
}

static const TypeInfo mcxn_powerquad_types[] = {
    {
        .name          = TYPE_MCXN_POWERQUAD,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPowerQuadState),
        .class_init    = mcxn_powerquad_class_init,
    },
};

DEFINE_TYPES(mcxn_powerquad_types)
