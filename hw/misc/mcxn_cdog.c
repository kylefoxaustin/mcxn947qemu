/*
 * NXP MCX N CDOG (Code Watchdog) — faithful register model.
 *
 * The CDOG is a deterministic code-flow watchdog: firmware programs CONTROL
 * and RELOAD, issues a START command, then periodically issues ADD/SUB/etc.
 * "instruction" commands to keep an internal counter in range.  A mismatch
 * or timeout would normally raise a fault/reset.  For bring-up we model the
 * register file faithfully but never run the timer, so:
 *   - command registers (START, STOP, RESTART, ADDx, SUBx, ASSERT16, all
 *     __O) are accepted and have no side effect (no fault is ever raised);
 *   - INSTRUCTION_TIMER / STATUS / STATUS2 are read-only and read back the
 *     benign "idle, no faults" value;
 *   - FLAGS is write-1-to-clear;
 *   - CONTROL / RELOAD / PERSISTENT are read-write backing storage.
 *
 * Offsets/bits from the MCXN947 CMSIS header (CDOG_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_cdog.h"
#include "migration/vmstate.h"

/* Register offsets */
#define CDOG_CONTROL            0x00  /* RW */
#define CDOG_RELOAD             0x04  /* RW */
#define CDOG_INSTRUCTION_TIMER  0x08  /* RO */
#define CDOG_STATUS             0x10  /* RO */
#define CDOG_STATUS2            0x14  /* RO */
#define CDOG_FLAGS              0x18  /* RW, flags are W1C */
#define CDOG_PERSISTENT         0x1C  /* RW */
#define CDOG_START              0x20  /* WO command */
#define CDOG_STOP               0x24  /* WO command */
#define CDOG_RESTART            0x28  /* WO command */
#define CDOG_ADD                0x2C  /* WO command */
#define CDOG_ADD1               0x30  /* WO command */
#define CDOG_ADD16              0x34  /* WO command */
#define CDOG_ADD256             0x38  /* WO command */
#define CDOG_SUB                0x3C  /* WO command */
#define CDOG_SUB1               0x40  /* WO command */
#define CDOG_SUB16              0x44  /* WO command */
#define CDOG_SUB256             0x48  /* WO command */
#define CDOG_ASSERT16           0x4C  /* WO command */

/* FLAGS write-1-to-clear bits (TO/MISCOM/SEQ/CNT/STATE/ADDR + POR). */
#define CDOG_FLAGS_W1C_MASK     0x0001003Fu

static uint64_t mcxn_cdog_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNCDOGState *s = MCXN_CDOG(opaque);

    switch (offset) {
    case CDOG_CONTROL:
    case CDOG_RELOAD:
    case CDOG_FLAGS:
    case CDOG_PERSISTENT:
        return s->regs[offset / 4];
    case CDOG_INSTRUCTION_TIMER:
        /* Timer not running; mirror RELOAD as the current count. */
        return s->regs[CDOG_RELOAD / 4];
    case CDOG_STATUS:
    case CDOG_STATUS2:
        /* No timeouts/miscompares/illegal states recorded. */
        return 0;
    default:
        /* Command registers (__O) read as 0. */
        if (offset <= CDOG_ASSERT16) {
            return 0;
        }
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void mcxn_cdog_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNCDOGState *s = MCXN_CDOG(opaque);

    switch (offset) {
    case CDOG_CONTROL:
    case CDOG_RELOAD:
    case CDOG_PERSISTENT:
        s->regs[offset / 4] = value;
        break;
    case CDOG_FLAGS:
        /* Write-1-to-clear the sticky fault flags. */
        s->regs[CDOG_FLAGS / 4] &= ~((uint32_t)value & CDOG_FLAGS_W1C_MASK);
        break;
    case CDOG_INSTRUCTION_TIMER:
    case CDOG_STATUS:
    case CDOG_STATUS2:
        /* Read-only. */
        break;
    case CDOG_START:
    case CDOG_STOP:
    case CDOG_RESTART:
    case CDOG_ADD:
    case CDOG_ADD1:
    case CDOG_ADD16:
    case CDOG_ADD256:
    case CDOG_SUB:
    case CDOG_SUB1:
    case CDOG_SUB16:
    case CDOG_SUB256:
    case CDOG_ASSERT16:
        /* Command registers: accepted, no fault is raised in the model. */
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps mcxn_cdog_ops = {
    .read = mcxn_cdog_read,
    .write = mcxn_cdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_cdog_reset(DeviceState *dev)
{
    MCXNCDOGState *s = MCXN_CDOG(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_cdog_realize(DeviceState *dev, Error **errp)
{
    MCXNCDOGState *s = MCXN_CDOG(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_cdog_ops, s,
                          TYPE_MCXN_CDOG, MCXN_CDOG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_cdog = {
    .name = TYPE_MCXN_CDOG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNCDOGState, MCXN_CDOG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_cdog_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_cdog_realize;
    device_class_set_legacy_reset(dc, mcxn_cdog_reset);
    dc->vmsd = &vmstate_mcxn_cdog;
}

static const TypeInfo mcxn_cdog_types[] = {
    {
        .name          = TYPE_MCXN_CDOG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCDOGState),
        .class_init    = mcxn_cdog_class_init,
    },
};

DEFINE_TYPES(mcxn_cdog_types)
