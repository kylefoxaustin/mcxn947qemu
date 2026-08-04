/*
 * NXP MCX N FREQME (Frequency Measurement) — bring-up model.
 *
 * Register map (FREQME_Type, base 0x40011000), all reset to 0:
 *   0x0  CTRL_R   (__I)  RESULT[30:0], MEASURE_IN_PROGRESS[31]
 *        CTRL_W   (__O)  config: REF_SCALE, *_INT_EN, CONTINUOUS_MODE_EN,
 *                        MEASURE_IN_PROGRESS[31] (write 1 to start)
 *   0x4  CTRLSTAT (__IO) config mirror of CTRL_W plus W1C status bits
 *                        RESULT_READY_STAT[26], GT_MAX_STAT[25],
 *                        LT_MIN_STAT[24]
 *   0x8  MIN      (__IO)
 *   0xC  MAX      (__IO)
 *
 * Behavior: a real measurement counts the target clock for 2^REF_SCALE
 * reference cycles.  There is no real clock here, so measurements complete
 * instantly: writing CTRL_W[MEASURE_IN_PROGRESS]=1 immediately leaves
 * MEASURE_IN_PROGRESS reading 0 (done) and sets CTRLSTAT[RESULT_READY_STAT].
 * RESULT reads back 0.  Drivers that poll MEASURE_IN_PROGRESS for completion or
 * RESULT_READY_STAT therefore make progress.  Per the RM, writing
 * CTRL_W[MEASURE_IN_PROGRESS]=0 terminates/resets RESULT.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_freqme.h"
#include "migration/vmstate.h"

#define FREQME_CTRL      0x0   /* CTRL_R (read) / CTRL_W (write) */
#define FREQME_CTRLSTAT  0x4
#define FREQME_MIN       0x8
#define FREQME_MAX       0xc

#define FREQME_RESULT_MASK               0x7fffffffu
#define FREQME_MEASURE_IN_PROGRESS       (1u << 31)

/*
 * CTRL_W / CTRLSTAT shared configuration bits:
 *   REF_SCALE[4:0], PULSE_MODE[8], PULSE_POL[9], LT_MIN_INT_EN[12],
 *   GT_MAX_INT_EN[13], RESULT_READY_INT_EN[14], CONTINUOUS_MODE_EN[30].
 * (MEASURE_IN_PROGRESS[31] is handled separately and reads back as 0 since
 * measurements complete instantly.)
 */
#define FREQME_CFG_MASK                  0x4000731fu

/* CTRLSTAT W1C status bits. */
#define FREQME_LT_MIN_STAT               (1u << 24)
#define FREQME_GT_MAX_STAT               (1u << 25)
#define FREQME_RESULT_READY_STAT         (1u << 26)
#define FREQME_STAT_MASK \
    (FREQME_LT_MIN_STAT | FREQME_GT_MAX_STAT | FREQME_RESULT_READY_STAT)

static uint64_t mcxn_freqme_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNFreqmeState *s = MCXN_FREQME(opaque);

    switch (offset) {
    case FREQME_CTRL:
        /*
         * CTRL_R: measurement always complete (never in progress); return the
         * last result in bits [30:0] with MEASURE_IN_PROGRESS cleared.
         */
        return s->result & FREQME_RESULT_MASK;
    case FREQME_CTRLSTAT:
        return s->ctrlstat;
    case FREQME_MIN:
        return s->min;
    case FREQME_MAX:
        return s->max;
    default:
        return 0;
    }
}

static void mcxn_freqme_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MCXNFreqmeState *s = MCXN_FREQME(opaque);
    uint32_t val = value;

    switch (offset) {
    case FREQME_CTRL:
        /*
         * CTRL_W: latch configuration into both CTRL and the CTRLSTAT
         * mirror.
         */
        s->ctrl = val & (FREQME_CFG_MASK | FREQME_MEASURE_IN_PROGRESS);
        s->ctrlstat = (s->ctrlstat & FREQME_STAT_MASK) |
                      (val & FREQME_CFG_MASK);

        if (val & FREQME_MEASURE_IN_PROGRESS) {
            /*
             * Start: completes instantly -> flag result ready, keep
             * RESULT 0.
             */
            s->result = 0;
            s->ctrlstat |= FREQME_RESULT_READY_STAT;
        } else {
            /* Terminate/idle: reset RESULT. */
            s->result = 0;
        }
        break;
    case FREQME_CTRLSTAT:
        /* Config bits writable; status bits [26:24] are write-1-to-clear. */
        s->ctrlstat = (s->ctrlstat & ~FREQME_STAT_MASK & ~FREQME_CFG_MASK)
                      | (val & FREQME_CFG_MASK)
                      | (s->ctrlstat & FREQME_STAT_MASK
                         & ~(val & FREQME_STAT_MASK));
        break;
    case FREQME_MIN:
        s->min = val;
        break;
    case FREQME_MAX:
        s->max = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mcxn_freqme_ops = {
    .read = mcxn_freqme_read,
    .write = mcxn_freqme_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_freqme_reset(DeviceState *dev)
{
    MCXNFreqmeState *s = MCXN_FREQME(dev);

    s->ctrl = 0;
    s->ctrlstat = 0;
    s->result = 0;
    s->min = 0;
    /*
     * ⚠ MAX IS THE MEASUREMENT CEILING, AND OURS RESET TO ZERO.  RM:
     *   0x7FFF_FFFF.  A ceiling of zero is exceeded by ANY measurement --
     *   so a guest reading MAX to size its window, or comparing a result
     *   against it, is comparing against a boundary the silicon never has.
     *   A dangerous zero: legal, meaningful, wrong.
     */
    s->max = 0x7FFFFFFFu;
}

static void mcxn_freqme_realize(DeviceState *dev, Error **errp)
{
    MCXNFreqmeState *s = MCXN_FREQME(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_freqme_ops, s,
                          TYPE_MCXN_FREQME, MCXN_FREQME_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_freqme = {
    .name = TYPE_MCXN_FREQME,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, MCXNFreqmeState),
        VMSTATE_UINT32(ctrlstat, MCXNFreqmeState),
        VMSTATE_UINT32(result, MCXNFreqmeState),
        VMSTATE_UINT32(min, MCXNFreqmeState),
        VMSTATE_UINT32(max, MCXNFreqmeState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_freqme_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_freqme_realize;
    device_class_set_legacy_reset(dc, mcxn_freqme_reset);
    dc->vmsd = &vmstate_mcxn_freqme;
}

static const TypeInfo mcxn_freqme_types[] = {
    {
        .name          = TYPE_MCXN_FREQME,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNFreqmeState),
        .class_init    = mcxn_freqme_class_init,
    },
};

DEFINE_TYPES(mcxn_freqme_types)
