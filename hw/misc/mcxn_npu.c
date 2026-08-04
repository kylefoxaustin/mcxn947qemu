/*
 * NXP MCX N NPX0 / eIQ Neutron NPU (neural accelerator) - bring-up model.
 *
 * The 0x400C_C000 window is named NPX0 in the MCXN947 CMSIS header, where the
 * documented NPX_Type is a flash-cache obfuscation block (NPXCR/NPXSR/CACMSK/
 * REMAP/context IV) rather than the Neutron NPU compute register set, which is
 * not broken out in CMSIS.  This is therefore a permissive 0x1000 register
 * array with readback, plus firmware-safe status semantics:
 *
 *   - The control word's soft-reset request self-clears (reads back 0).
 *   - The status word always reports not-busy / done / ready.
 *   - The interrupt-status word is write-1-to-clear.
 *
 * The two documented NPX offsets that gate firmware (NPXCR control
 * reflect-back, NPXSR status read) are honoured by these conventions.
 * See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_npu.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/*
 * Register offsets.  The control/status/interrupt triad below follows the
 * documented NPX_Type layout (NPXCR @0x0, NPXSR @0x8) and a conventional
 * accelerator interrupt-status word; all other offsets fall through to the
 * permissive backing array.
 */
#define R_CR        0x00  /* control: reset request self-clears */
#define R_SR        0x08  /* status: always reads idle/done/ready */
#define R_IRQSTAT   0x0C  /* interrupt status: write-1-to-clear */

/*
 * Soft-reset / start request bits.  The Neutron compute register set is not
 * documented in CMSIS, so a broad low-bit mask is cleared on read so any "kick
 * then wait for self-clear" idiom completes regardless of the exact bit.
 */
#define CR_SELFCLEAR_MASK  0x0000000Fu

/*
 * Status idle/done convention: report busy bits low and a done/ready bit high.
 * Bit 0 reads as ready/done; the busy-class bits (1..3) read low.
 */
#define SR_READY           (1u << 0)
#define SR_BUSY_MASK       0x0000000Eu

static uint64_t mcxn_npu_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNNPUState *s = MCXN_NPU(opaque);
    uint32_t v = (off < MCXN_NPU_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case R_CR:
        /*
         * Any soft-reset/start request has already retired: read it back
         * low.
         */
        return v & ~CR_SELFCLEAR_MASK;
    case R_SR:
        /* Never busy: clear the busy-class bits and report ready/done. */
        return (v & ~SR_BUSY_MASK) | SR_READY;
    default:
        return v;
    }
}

static void mcxn_npu_write(void *opaque, hwaddr off,
                           uint64_t value, unsigned size)
{
    MCXNNPUState *s = MCXN_NPU(opaque);
    uint32_t v = value;

    if (off >= MCXN_NPU_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_CR:
        /* Reflect control back but retire the reset/start request instantly. */
        s->regs[off >> 2] = v & ~CR_SELFCLEAR_MASK;
        return;
    case R_SR:
        return;                 /* status is read-only */
    case R_IRQSTAT:
        /* Interrupt status is write-1-to-clear. */
        s->regs[off >> 2] &= ~v;
        qemu_set_irq(s->irq, 0);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_npu_ops = {
    .read = mcxn_npu_read,
    .write = mcxn_npu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_npu_reset(DeviceState *dev)
{
    MCXNNPUState *s = MCXN_NPU(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void mcxn_npu_realize(DeviceState *dev, Error **errp)
{
    MCXNNPUState *s = MCXN_NPU(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_npu_ops, s,
                          TYPE_MCXN_NPU, MCXN_NPU_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_npu = {
    .name = TYPE_MCXN_NPU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNNPUState, MCXN_NPU_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_npu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_npu_realize;
    device_class_set_legacy_reset(dc, mcxn_npu_reset);
    dc->vmsd = &vmstate_mcxn_npu;
}

static const TypeInfo mcxn_npu_types[] = {
    {
        .name          = TYPE_MCXN_NPU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNNPUState),
        .class_init    = mcxn_npu_class_init,
    },
};

DEFINE_TYPES(mcxn_npu_types)
