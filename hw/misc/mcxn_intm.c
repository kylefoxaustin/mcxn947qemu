/*
 * NXP MCX N INTM (Interrupt Monitor) — register-accurate model.
 *
 * The INTM measures the latency between an interrupt request and its
 * acknowledge for up to four monitors and raises a fault if a programmed timer
 * is exceeded.  Register layout (INTM_Type):
 *
 *   0x0   INTM_MM            RW   Monitor Mode
 *   0x4   INTM_IACK          WO   Interrupt Acknowledge
 *   per monitor n (0..3), base 0x8 + n*0x10:
 *     +0x0  INTM_IRQSELn     RW   Interrupt Request Select
 *     +0x4  INTM_LATENCYn    RW   Interrupt Latency
 *     +0x8  INTM_TIMERn      RW   Timer
 *     +0xC  INTM_STATUSn     RO   Status
 *
 * In emulation the monitor never fires, so the read-only STATUSn registers read
 * 0 ("no latency violation") and the write-only IACK register reads 0.  All
 * registers reset to 0 (RM section 28.6).  Offsets/access-types from the
 * MCXN947 CMSIS header (INTM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_intm.h"
#include "migration/vmstate.h"

#define INTM_MM     0x00   /* RW  Monitor Mode */
#define INTM_IACK   0x04   /* WO  Interrupt Acknowledge */

/* Per-monitor sub-block: 4 monitors of 0x10 bytes starting at 0x8. */
#define INTM_MON_BASE     0x08
#define INTM_MON_STEP     0x10
#define INTM_MON_COUNT    4
#define INTM_MON_STATUS   0x0C   /* RO  Status within a monitor sub-block */

static bool intm_is_status(hwaddr offset)
{
    if (offset >= INTM_MON_BASE) {
        hwaddr rel = offset - INTM_MON_BASE;

        if (rel < INTM_MON_COUNT * INTM_MON_STEP) {
            return (rel % INTM_MON_STEP) == INTM_MON_STATUS;
        }
    }
    return false;
}

static uint64_t mcxn_intm_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNINTMState *s = MCXN_INTM(opaque);

    if (offset == INTM_IACK) {
        /* Write-only register: reads return 0. */
        return 0;
    }
    /* STATUSn is read-only and always 0 (no monitor fires in emulation). */
    return s->regs[offset / 4];
}

static void mcxn_intm_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNINTMState *s = MCXN_INTM(opaque);

    if (intm_is_status(offset)) {
        return;  /* STATUSn is read-only */
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_intm_ops = {
    .read = mcxn_intm_read,
    .write = mcxn_intm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_intm_reset(DeviceState *dev)
{
    MCXNINTMState *s = MCXN_INTM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_intm_realize(DeviceState *dev, Error **errp)
{
    MCXNINTMState *s = MCXN_INTM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_intm_ops, s,
                          TYPE_MCXN_INTM, MCXN_INTM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_intm = {
    .name = TYPE_MCXN_INTM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNINTMState, MCXN_INTM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_intm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_intm_realize;
    device_class_set_legacy_reset(dc, mcxn_intm_reset);
    dc->vmsd = &vmstate_mcxn_intm;
}

static const TypeInfo mcxn_intm_types[] = {
    {
        .name          = TYPE_MCXN_INTM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNINTMState),
        .class_init    = mcxn_intm_class_init,
    },
};

DEFINE_TYPES(mcxn_intm_types)
