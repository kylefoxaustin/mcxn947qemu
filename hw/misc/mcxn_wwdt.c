/*
 * NXP MCX N WWDT (Windowed Watchdog Timer) — bring-up model.
 *
 * The windowed watchdog is modelled as a register file so that firmware can
 * configure it (MOD/TC/WINDOW/WARNINT) and feed it (FEED) without the model
 * ever asserting a watchdog timeout or chip reset.  TC holds the reload value;
 * the read-only TV (timer value) reports the configured reload value so a driver
 * reading the down-counter sees a sane, non-zero value.  FEED is write-only and
 * reloads the (virtual) counter — no reset is generated.  The MOD.WDTOF /
 * MOD.WDINT status flags are never set because no timeout is ever produced.
 * One shared type drives two instances (WWDT0, WWDT1).  Offsets and reset
 * values from the MCXN947 CMSIS header (WWDT_Type) and the reference manual.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_wwdt.h"
#include "migration/vmstate.h"

#define WWDT_MOD      0x00
#define WWDT_TC       0x04
#define WWDT_FEED     0x08  /* WO */
#define WWDT_TV       0x0C  /* RO: timer value */
#define WWDT_WARNINT  0x14
#define WWDT_WINDOW   0x18

/* Reset values (reference manual). */
#define WWDT_TC_RESET      0x000000FFu
#define WWDT_TV_RESET      0x000000FFu
#define WWDT_WINDOW_RESET  0x00FFFFFFu

/* MOD status flags that this model never sets (no timeout is produced). */
#define WWDT_MOD_WDTOF     (1u << 2)
#define WWDT_MOD_WDINT     (1u << 3)

static uint64_t mcxn_wwdt_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNWWDTState *s = MCXN_WWDT(opaque);

    if (off >= MCXN_WWDT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off) {
    case WWDT_TV:
        /*
         * The watchdog never counts down in the model: report the reload value
         * (TC) so a driver that reads the running counter sees a plausible,
         * non-zero, in-window value.  Falls back to the TV reset value if TC is
         * still zero.
         */
        if (s->regs[WWDT_TC / 4] != 0) {
            return s->regs[WWDT_TC / 4];
        }
        return WWDT_TV_RESET;
    case WWDT_FEED:
        return 0;  /* write-only */
    default:
        return s->regs[off >> 2];
    }
}

static void mcxn_wwdt_write(void *opaque, hwaddr off,
                            uint64_t value, unsigned size)
{
    MCXNWWDTState *s = MCXN_WWDT(opaque);

    if (off >= MCXN_WWDT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case WWDT_TV:
        return;  /* read-only */
    case WWDT_FEED:
        /*
         * A valid feed sequence (0xAA then 0x55) reloads the counter.  No reset
         * is ever generated; nothing further to model.
         */
        return;
    case WWDT_MOD:
        /*
         * WDTOF/WDINT are status flags; the model never asserts a timeout, so
         * keep them clear regardless of what software writes.
         */
        s->regs[WWDT_MOD / 4] = (uint32_t)value & ~(WWDT_MOD_WDTOF |
                                                    WWDT_MOD_WDINT);
        return;
    default:
        s->regs[off >> 2] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_wwdt_ops = {
    .read = mcxn_wwdt_read,
    .write = mcxn_wwdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_wwdt_reset(DeviceState *dev)
{
    MCXNWWDTState *s = MCXN_WWDT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[WWDT_TC / 4]     = WWDT_TC_RESET;
    s->regs[WWDT_WINDOW / 4] = WWDT_WINDOW_RESET;
}

static void mcxn_wwdt_realize(DeviceState *dev, Error **errp)
{
    MCXNWWDTState *s = MCXN_WWDT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_wwdt_ops, s,
                          TYPE_MCXN_WWDT, MCXN_WWDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_wwdt = {
    .name = TYPE_MCXN_WWDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNWWDTState, MCXN_WWDT_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_wwdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_wwdt_realize;
    device_class_set_legacy_reset(dc, mcxn_wwdt_reset);
    dc->vmsd = &vmstate_mcxn_wwdt;
}

static const TypeInfo mcxn_wwdt_types[] = {
    {
        .name          = TYPE_MCXN_WWDT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNWWDTState),
        .class_init    = mcxn_wwdt_class_init,
    },
};

DEFINE_TYPES(mcxn_wwdt_types)
