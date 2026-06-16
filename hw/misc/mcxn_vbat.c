/*
 * NXP MCX N VBAT (VBAT / always-on domain) — bring-up model.
 *
 * The VBAT block hosts the always-on (RTC backup) resources: the 16 kHz FRO and
 * the 32 kHz crystal oscillator, the RAM LDO and the tamper/CLKMON logic.
 * Firmware that enables the oscillator or LDO polls STATUSA for the matching
 * "ready" bit; the model reports those bits ready so init never spins. STATUSA
 * and STATUSB carry sticky flag bits that are write-1-to-clear. VERID returns a
 * constant. Remaining registers are backed permissively. Offsets/bits from the
 * MCXN947 CMSIS header (VBAT_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_vbat.h"
#include "migration/vmstate.h"

/* Register offsets */
#define VBAT_VERID    0x00   /* RO */
#define VBAT_STATUSA  0x10
#define VBAT_STATUSB  0x14

/*
 * STATUSA status/ready bits. LDO_RDY and OSC_RDY are level "ready" indicators
 * the driver polls after enabling the LDO/oscillator; the flag bits below are
 * sticky write-1-to-clear event flags.
 */
#define VBAT_STATUSA_POR_DET      (1u << 0)
#define VBAT_STATUSA_WAKEUP_FLAG  (1u << 1)
#define VBAT_STATUSA_TIMER0_FLAG  (1u << 2)
#define VBAT_STATUSA_TIMER1_FLAG  (1u << 3)
#define VBAT_STATUSA_LDO_RDY      (1u << 4)
#define VBAT_STATUSA_OSC_RDY      (1u << 5)

/* Sticky, write-1-to-clear flag bits in STATUSA. */
#define VBAT_STATUSA_W1C \
    (VBAT_STATUSA_POR_DET | VBAT_STATUSA_WAKEUP_FLAG | \
     VBAT_STATUSA_TIMER0_FLAG | VBAT_STATUSA_TIMER1_FLAG | \
     (1u << 6)  /* CLOCK_DET  */ | \
     (1u << 7)  /* CONFIG_DET */ | \
     (1u << 8)  /* VOLT_DET   */ | \
     (1u << 9)  /* TEMP_DET   */ | \
     (1u << 10) /* LIGHT_DET  */ | \
     (1u << 12) /* SEC0_DET   */ | \
     (1u << 16) /* IRQ0_DET   */ | \
     (1u << 17) /* IRQ1_DET   */ | \
     (1u << 18) /* IRQ2_DET   */ | \
     (1u << 19) /* IRQ3_DET   */)

/* Level "ready" bits that always read ready in the model. */
#define VBAT_STATUSA_READY (VBAT_STATUSA_LDO_RDY | VBAT_STATUSA_OSC_RDY)

#define VBAT_VERID_VALUE  0x02000000u

static uint64_t mcxn_vbat_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNVBATState *s = MCXN_VBAT(opaque);
    uint32_t v = (off < MCXN_VBAT_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case VBAT_VERID:
        return VBAT_VERID_VALUE;
    case VBAT_STATUSA:
        /* Oscillator and LDO are always ready in the model. */
        return v | VBAT_STATUSA_READY;
    default:
        return v;
    }
}

static void mcxn_vbat_write(void *opaque, hwaddr off,
                            uint64_t value, unsigned size)
{
    MCXNVBATState *s = MCXN_VBAT(opaque);

    if (off >= MCXN_VBAT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case VBAT_VERID:
        return;  /* read-only */
    case VBAT_STATUSA:
        /* Write-1-to-clear the sticky flag bits; ready bits are not stored. */
        s->regs[off >> 2] &= ~((uint32_t)value & VBAT_STATUSA_W1C);
        return;
    case VBAT_STATUSB:
        /* STATUSB mirrors STATUSA flags (inverse); treat as write-1-to-clear. */
        s->regs[off >> 2] &= ~(uint32_t)value;
        return;
    default:
        s->regs[off >> 2] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_vbat_ops = {
    .read = mcxn_vbat_read,
    .write = mcxn_vbat_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_vbat_reset(DeviceState *dev)
{
    MCXNVBATState *s = MCXN_VBAT(dev);
    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_vbat_realize(DeviceState *dev, Error **errp)
{
    MCXNVBATState *s = MCXN_VBAT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_vbat_ops, s,
                          TYPE_MCXN_VBAT, MCXN_VBAT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_vbat = {
    .name = TYPE_MCXN_VBAT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNVBATState, MCXN_VBAT_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_vbat_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_vbat_realize;
    device_class_set_legacy_reset(dc, mcxn_vbat_reset);
    dc->vmsd = &vmstate_mcxn_vbat;
}

static const TypeInfo mcxn_vbat_types[] = {
    {
        .name          = TYPE_MCXN_VBAT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNVBATState),
        .class_init    = mcxn_vbat_class_init,
    },
};

DEFINE_TYPES(mcxn_vbat_types)
