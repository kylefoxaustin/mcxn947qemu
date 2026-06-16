/*
 * NXP MCX N CMX_PERFMON (Performance Monitor) — register-accurate model.
 *
 * The performance monitor (CMSIS SYSPM_Type) holds a control register (PMCR)
 * and three 40-bit event counters, each split into an 8-bit HI byte and a
 * 32-bit LO word.  In emulation no events are actually counted, so the
 * read-only counters simply read back 0 (or whatever a debugger left there;
 * they are reset to 0).  PMCR is read/write and stored verbatim; its self-
 * clearing reset/start/stop bits have no externally-observable effect here.
 *
 *   - PMCR is read/write (CMSIS __IO).
 *   - PMECTRn_HI / PMECTRn_LO are read-only (CMSIS __I); counters read 0.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (SYSPM_Type); reset
 * values from the MCX N Reference Manual (chapter 11, all 0).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_cmx_perfmon.h"
#include "migration/vmstate.h"

#define PERFMON_PMCR        0x00   /* RW  Performance Monitor Control */

/*
 * Event counters, array step 0x8 within the 0x30 PMCR block.  HI is a single
 * read-only byte; LO is a read-only word.
 */
#define PERFMON_PMECTR0_HI  0x18   /* RO  byte */
#define PERFMON_PMECTR0_LO  0x1C   /* RO  word */
#define PERFMON_PMECTR1_HI  0x20   /* RO  byte */
#define PERFMON_PMECTR1_LO  0x24   /* RO  word */
#define PERFMON_PMECTR2_HI  0x28   /* RO  byte */
#define PERFMON_PMECTR2_LO  0x2C   /* RO  word */

static bool perfmon_is_counter(hwaddr offset)
{
    switch (offset) {
    case PERFMON_PMECTR0_HI:
    case PERFMON_PMECTR0_LO:
    case PERFMON_PMECTR1_HI:
    case PERFMON_PMECTR1_LO:
    case PERFMON_PMECTR2_HI:
    case PERFMON_PMECTR2_LO:
        return true;
    default:
        return false;
    }
}

static uint64_t mcxn_cmx_perfmon_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    MCXNCMXPerfmonState *s = MCXN_CMX_PERFMON(opaque);

    if (perfmon_is_counter(offset)) {
        /* Read-only event counters: nothing is counted in the model. */
        return 0;
    }
    return s->regs[offset / 4];
}

static void mcxn_cmx_perfmon_write(void *opaque, hwaddr offset, uint64_t value,
                                   unsigned size)
{
    MCXNCMXPerfmonState *s = MCXN_CMX_PERFMON(opaque);

    if (perfmon_is_counter(offset)) {
        /* Read-only counters: ignore writes. */
        return;
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_cmx_perfmon_ops = {
    .read = mcxn_cmx_perfmon_read,
    .write = mcxn_cmx_perfmon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_cmx_perfmon_reset(DeviceState *dev)
{
    MCXNCMXPerfmonState *s = MCXN_CMX_PERFMON(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_cmx_perfmon_realize(DeviceState *dev, Error **errp)
{
    MCXNCMXPerfmonState *s = MCXN_CMX_PERFMON(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_cmx_perfmon_ops, s,
                          TYPE_MCXN_CMX_PERFMON, MCXN_CMX_PERFMON_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_cmx_perfmon = {
    .name = TYPE_MCXN_CMX_PERFMON,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNCMXPerfmonState,
                             MCXN_CMX_PERFMON_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_cmx_perfmon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_cmx_perfmon_realize;
    device_class_set_legacy_reset(dc, mcxn_cmx_perfmon_reset);
    dc->vmsd = &vmstate_mcxn_cmx_perfmon;
}

static const TypeInfo mcxn_cmx_perfmon_types[] = {
    {
        .name          = TYPE_MCXN_CMX_PERFMON,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCMXPerfmonState),
        .class_init    = mcxn_cmx_perfmon_class_init,
    },
};

DEFINE_TYPES(mcxn_cmx_perfmon_types)
