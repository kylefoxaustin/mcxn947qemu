/*
 * NXP MCX N SMARTDMA (programmable DMA coprocessor) - bring-up model.  See
 * header for design notes.  Offsets and bits from the MCXN947 CMSIS header
 * (SMARTDMA_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_smartdma.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS SMARTDMA_Type). */
#define R_BOOTADR     0x20  /* RW: boot address */
#define R_CTRL        0x24  /* RW: control (START in bit 0) */
#define R_PC          0x28  /* RO: program counter */
#define R_SP          0x2C  /* RO: stack pointer */
#define R_BREAK_ADDR  0x30  /* RW */
#define R_BREAK_VECT  0x34  /* RW */
#define R_EMER_VECT   0x38  /* RW */
#define R_EMER_SEL    0x3C  /* RW */
#define R_ARM2EZH     0x40  /* RW: ARM-to-EZH interrupt control */
#define R_EZH2ARM     0x44  /* RW: EZH-to-ARM trigger (W1C-style status) */
#define R_PENDTRAP    0x48  /* RW: pending trap control (STATUS is W1C) */

#define CTRL_START    (1u << 0)  /* start bit ignition (self-clears here) */

/* ARM2EZH[1:0] selects the EZH-to-ARM signalling mode. */
#define ARM2EZH_IE_MASK  0x3u

/* PENDTRAP.STATUS is the pending-trap request flag set (W1C). */
#define PENDTRAP_STATUS_MASK  0xFFu

static uint64_t mcxn_smartdma_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(opaque);
    uint32_t v = (off < MCXN_SMARTDMA_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case R_CTRL:
        /*
         * The engine completes instantly in the model: never report START as
         * still pending so a boot-then-poll loop falls through.
         */
        return v & ~CTRL_START;
    case R_PC:
    case R_SP:
        /* Read-only engine state; nothing to model, report the boot address. */
        return s->regs[R_BOOTADR >> 2];
    default:
        return v;
    }
}

static void mcxn_smartdma_write(void *opaque, hwaddr off,
                                uint64_t value, unsigned size)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(opaque);
    uint32_t v = value;

    if (off >= MCXN_SMARTDMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_PC:
    case R_SP:
        return;                 /* read-only engine state */
    case R_CTRL:
        /* Reflect control back but immediately retire the START request. */
        s->regs[off >> 2] = v & ~CTRL_START;
        return;
    case R_EZH2ARM:
        /*
         * Writing EZH2ARM is the engine-to-ARM trigger; when ARM2EZH[1:0] == 2h
         * it would raise the ARM interrupt.  Latch the value but keep the line
         * deasserted (no real engine is running to drive it).
         */
        s->regs[off >> 2] = v;
        qemu_set_irq(s->irq, 0);
        return;
    case R_PENDTRAP:
        /* STATUS field (bits 7:0) is write-1-to-clear; other fields reflect. */
        s->regs[off >> 2] = (s->regs[off >> 2] & ~(v & PENDTRAP_STATUS_MASK))
                          | (v & ~PENDTRAP_STATUS_MASK);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_smartdma_ops = {
    .read = mcxn_smartdma_read,
    .write = mcxn_smartdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_smartdma_reset(DeviceState *dev)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void mcxn_smartdma_realize(DeviceState *dev, Error **errp)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_smartdma_ops, s,
                          TYPE_MCXN_SMARTDMA, MCXN_SMARTDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_smartdma = {
    .name = TYPE_MCXN_SMARTDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSmartDMAState, MCXN_SMARTDMA_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_smartdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_smartdma_realize;
    device_class_set_legacy_reset(dc, mcxn_smartdma_reset);
    dc->vmsd = &vmstate_mcxn_smartdma;
}

static const TypeInfo mcxn_smartdma_types[] = {
    {
        .name          = TYPE_MCXN_SMARTDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSmartDMAState),
        .class_init    = mcxn_smartdma_class_init,
    },
};

DEFINE_TYPES(mcxn_smartdma_types)
