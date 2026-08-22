/*
 * NXP MCX N MAILBOX (Inter-CPU Mailbox) — register-accurate model.
 *
 * The mailbox provides a handshake between CPU0 (Cortex-M33) and CPU1
 * (CoolFlux).  Each direction owns an MBOXIRQ[n] triplet:
 *   - IRQ    (offset 0x00 / 0x10): read/write pending interrupt-request word.
 *   - IRQSET (offset 0x04 / 0x14): write-only; set bits in IRQ (read 0).
 *   - IRQCLR (offset 0x08 / 0x18): write-only; clear bits in IRQ (read 0).
 * A single MUTEX register (offset 0xF8) implements the resource handshake:
 * MUTEX[EX] reads the current availability and then becomes 0 (so the reader
 * that observes 1 has taken the lock); any write makes it 1 again (release).
 *
 * Each CPU's IRQ word, when non-zero, asserts that CPU's mailbox interrupt
 * (MAILBOX_IRQn = 54 on its own NVIC): IRQ[0] -> cpu0, IRQ[1] -> cpu1.  One
 * core signals the other by writing the other's IRQSET, so this is a
 * genuine cross-core interrupt — the notification mechanism OpenAMP/rpmsg
 * rides on.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (MAILBOX_Type); reset
 * values from the MCX N Reference Manual (chapter 22): IRQ words 0,
 * MUTEX[EX] = 1 (resource available).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_mailbox.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define MAILBOX_IRQ0     0x00   /* RW  CPU0 interrupt request */
#define MAILBOX_IRQSET0  0x04   /* WO  CPU0 interrupt set */
#define MAILBOX_IRQCLR0  0x08   /* WO  CPU0 interrupt clear */
#define MAILBOX_IRQ1     0x10   /* RW  CPU1 interrupt request */
#define MAILBOX_IRQSET1  0x14   /* WO  CPU1 interrupt set */
#define MAILBOX_IRQCLR1  0x18   /* WO  CPU1 interrupt clear */
#define MAILBOX_MUTEX    0xF8   /* RW  Mutual Exclusion */

#define MAILBOX_MUTEX_EX (1u << 0)

/* Each CPU's IRQ word, when non-zero, drives that CPU's mailbox NVIC line. */
static void mcxn_mailbox_update_irq(MCXNMailboxState *s)
{
    qemu_set_irq(s->out[0], s->irq[0] != 0);
    qemu_set_irq(s->out[1], s->irq[1] != 0);
}

static uint64_t mcxn_mailbox_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNMailboxState *s = MCXN_MAILBOX(opaque);
    uint32_t val;

    switch (offset) {
    case MAILBOX_IRQ0:
        return s->irq[0];
    case MAILBOX_IRQ1:
        return s->irq[1];
    case MAILBOX_IRQSET0:
    case MAILBOX_IRQCLR0:
    case MAILBOX_IRQSET1:
    case MAILBOX_IRQCLR1:
        /* Write-only registers: reads return 0. */
        return 0;
    case MAILBOX_MUTEX:
        /*
         * Reading MUTEX returns the current availability and atomically clears
         * the EX bit: the reader that sees 1 has acquired the lock.
         */
        val = s->mutex & MAILBOX_MUTEX_EX;
        s->mutex &= ~MAILBOX_MUTEX_EX;
        return val;
    default:
        return 0;
    }
}

static void mcxn_mailbox_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    MCXNMailboxState *s = MCXN_MAILBOX(opaque);

    switch (offset) {
    case MAILBOX_IRQ0:
        s->irq[0] = value;
        mcxn_mailbox_update_irq(s);
        return;
    case MAILBOX_IRQ1:
        s->irq[1] = value;
        mcxn_mailbox_update_irq(s);
        return;
    case MAILBOX_IRQSET0:
        s->irq[0] |= value;
        mcxn_mailbox_update_irq(s);
        return;
    case MAILBOX_IRQCLR0:
        s->irq[0] &= ~(uint32_t)value;
        mcxn_mailbox_update_irq(s);
        return;
    case MAILBOX_IRQSET1:
        s->irq[1] |= value;
        mcxn_mailbox_update_irq(s);
        return;
    case MAILBOX_IRQCLR1:
        s->irq[1] &= ~(uint32_t)value;
        mcxn_mailbox_update_irq(s);
        return;
    case MAILBOX_MUTEX:
        /* Any write releases the resource: EX becomes 1 again. */
        s->mutex |= MAILBOX_MUTEX_EX;
        return;
    default:
        return;
    }
}

static const MemoryRegionOps mcxn_mailbox_ops = {
    .read = mcxn_mailbox_read,
    .write = mcxn_mailbox_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_mailbox_reset(DeviceState *dev)
{
    MCXNMailboxState *s = MCXN_MAILBOX(dev);

    s->irq[0] = 0;
    s->irq[1] = 0;
    s->mutex = MAILBOX_MUTEX_EX;   /* resource available at reset */
}

static void mcxn_mailbox_realize(DeviceState *dev, Error **errp)
{
    MCXNMailboxState *s = MCXN_MAILBOX(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_mailbox_ops, s,
                          TYPE_MCXN_MAILBOX, MCXN_MAILBOX_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->out[0]);  /* -> cpu0 NVIC[54] */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->out[1]);  /* -> cpu1 NVIC[54] */
}

/* Re-drive the output IRQ lines from restored register state after migration:
 * the output lines are not part of vmstate, so a VM migrated with a pending
 * mailbox IRQ would otherwise land with the lines low and the guest's level
 * IRQ lost. */
static int mcxn_mailbox_post_load(void *opaque, int version_id)
{
    mcxn_mailbox_update_irq(MCXN_MAILBOX(opaque));
    return 0;
}

static const VMStateDescription vmstate_mcxn_mailbox = {
    .name = TYPE_MCXN_MAILBOX,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mcxn_mailbox_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(irq, MCXNMailboxState, 2),
        VMSTATE_UINT32(mutex, MCXNMailboxState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_mailbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_mailbox_realize;
    device_class_set_legacy_reset(dc, mcxn_mailbox_reset);
    dc->vmsd = &vmstate_mcxn_mailbox;
}

static const TypeInfo mcxn_mailbox_types[] = {
    {
        .name          = TYPE_MCXN_MAILBOX,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNMailboxState),
        .class_init    = mcxn_mailbox_class_init,
    },
};

DEFINE_TYPES(mcxn_mailbox_types)
