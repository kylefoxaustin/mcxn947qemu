/*
 * NXP MCX N MRT (Multi-Rate Timer) — functional model.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/timer/mcxn_mrt.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"

#define CH_INTVAL 0x0
#define CH_TIMER  0x4
#define CH_CTRL   0x8
#define CH_STAT   0xC
#define CH_STRIDE 0x10

#define R_MODCFG   0xF0
#define R_IDLE_CH  0xF4
#define R_IRQ_FLAG 0xF8

#define INTVAL_LOAD   (1u << 31)
#define INTVAL_IVALUE 0x7FFFFFFFu
#define CTRL_INTEN    (1u << 0)
#define CTRL_MODE     (3u << 1)
#define MODE_REPEAT   0u
#define STAT_INTFLAG  (1u << 0)
#define STAT_RUN      (1u << 1)

static uint32_t mrt_freq(MCXNMRTState *s)
{
    /*
     * The bus clock, DERIVED from the SCG main clock (SoC wires it).  No
     * fallback: a `?: 150000000` here was the same camouflage the
     * CTIMER/OSTIMER carried -- it hid a missing clock behind a plausible
     * constant.  A bus clock the SCG reports as 0 (no main-clock source
     * selected) means the timer genuinely does not run, which is what
     * silicon does; substituting 150 MHz would be a silent wrong answer.
     */
    return s->clk ? clock_get_hz(s->clk) : 0;
}

/* Live down-count value of a channel at time `now`. */
static uint32_t mrt_peek(MCXNMRTState *s, int n, int64_t now)
{
    uint64_t elapsed, counts;

    if (!(s->stat[n] & STAT_RUN) || now <= s->base_ns[n]) {
        return s->load[n];
    }
    elapsed = now - s->base_ns[n];
    counts = (elapsed * mrt_freq(s)) / 1000000000ULL;
    return (counts >= s->load[n]) ? 0 : (s->load[n] - (uint32_t)counts);
}

static void mrt_update_irq(MCXNMRTState *s)
{
    int n, level = 0;

    for (n = 0; n < MCXN_MRT_CHANNELS; n++) {
        if ((s->stat[n] & STAT_INTFLAG) && (s->ctrl[n] & CTRL_INTEN)) {
            level = 1;
        }
    }
    qemu_set_irq(s->irq, level);
}

static void mrt_reschedule(MCXNMRTState *s, int64_t now)
{
    uint64_t best = UINT64_MAX, freq = mrt_freq(s);
    int n;

    timer_del(&s->timer);
    for (n = 0; n < MCXN_MRT_CHANNELS; n++) {
        uint32_t cur;
        uint64_t ns;

        if (!(s->stat[n] & STAT_RUN)) {
            continue;
        }
        cur = mrt_peek(s, n, now);
        ns = (cur * 1000000000ULL) / freq;
        if (ns < best) {
            best = ns;
        }
    }
    if (best != UINT64_MAX) {
        timer_mod(&s->timer, now + (int64_t)(best ? best : 1));
    }
}

static void mrt_tick(void *opaque)
{
    MCXNMRTState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int n;

    for (n = 0; n < MCXN_MRT_CHANNELS; n++) {
        if (!(s->stat[n] & STAT_RUN)) {
            continue;
        }
        if (mrt_peek(s, n, now) == 0) {
            s->stat[n] |= STAT_INTFLAG;
            if ((s->ctrl[n] & CTRL_MODE) >> 1 == MODE_REPEAT && s->intval[n]) {
                s->load[n] = s->intval[n] & INTVAL_IVALUE;  /* reload */
                s->base_ns[n] = now;
            } else {
                s->stat[n] &= ~STAT_RUN;                    /* one-shot stop */
                s->load[n] = 0;
            }
        }
    }
    mrt_update_irq(s);
    mrt_reschedule(s, now);
}

static uint64_t mrt_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNMRTState *s = MCXN_MRT(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int n;

    if (off < CH_STRIDE * MCXN_MRT_CHANNELS) {
        n = off / CH_STRIDE;
        switch (off % CH_STRIDE) {
        case CH_INTVAL: return s->intval[n] & INTVAL_IVALUE;
        case CH_TIMER:  return mrt_peek(s, n, now);
        case CH_CTRL:   return s->ctrl[n];
        case CH_STAT:   return s->stat[n];
        }
        return 0;
    }
    switch (off) {
    case R_IDLE_CH: {
        int idle = 0;
        for (n = 0; n < MCXN_MRT_CHANNELS; n++) {
            if (!(s->stat[n] & STAT_RUN)) {
                idle = n;
                break;
            }
        }
        return idle << 4;
    }
    case R_IRQ_FLAG: {
        uint32_t f = 0;
        for (n = 0; n < MCXN_MRT_CHANNELS; n++) {
            if (s->stat[n] & STAT_INTFLAG) {
                f |= (1u << n);
            }
        }
        return f;
    }
    case R_MODCFG: return 0;
    default:       return 0;
    }
}

static void mrt_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MCXNMRTState *s = MCXN_MRT(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t v = val;
    int n;

    if (off < CH_STRIDE * MCXN_MRT_CHANNELS) {
        n = off / CH_STRIDE;
        switch (off % CH_STRIDE) {
        case CH_INTVAL:
            s->intval[n] = v;
            /* Load now if LOAD bit set, or the channel is idle. */
            if ((v & INTVAL_LOAD) || !(s->stat[n] & STAT_RUN)) {
                s->load[n] = v & INTVAL_IVALUE;
                s->base_ns[n] = now;
                if (s->load[n]) {
                    s->stat[n] |= STAT_RUN;
                } else {
                    s->stat[n] &= ~STAT_RUN;
                }
            }
            mrt_reschedule(s, now);
            return;
        case CH_CTRL:
            s->ctrl[n] = v & 0x7;
            mrt_update_irq(s);
            return;
        case CH_STAT:
            if (v & STAT_INTFLAG) {            /* write-1-to-clear */
                s->stat[n] &= ~STAT_INTFLAG;
            }
            mrt_update_irq(s);
            return;
        }
        return;
    }
    /* global registers: accept writes, no side effects modelled */
}

static const MemoryRegionOps mrt_ops = {
    .read = mrt_read,
    .write = mrt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_mrt_reset(DeviceState *dev)
{
    MCXNMRTState *s = MCXN_MRT(dev);
    int n;

    timer_del(&s->timer);
    for (n = 0; n < MCXN_MRT_CHANNELS; n++) {
        s->intval[n] = s->ctrl[n] = s->stat[n] = s->load[n] = 0;
        s->base_ns[n] = 0;
    }
}

static void mcxn_mrt_init(Object *obj)
{
    MCXNMRTState *s = MCXN_MRT(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static void mcxn_mrt_realize(DeviceState *dev, Error **errp)
{
    MCXNMRTState *s = MCXN_MRT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mrt_ops, s,
                          TYPE_MCXN_MRT, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, mrt_tick, s);
}

/* Re-drive the IRQ line from restored register state after migration: the
 * output line is not part of vmstate, so a VM migrated with an asserted IRQ
 * would otherwise land with the line low and the guest's level IRQ lost. */
static int mcxn_mrt_post_load(void *opaque, int version_id)
{
    mrt_update_irq(MCXN_MRT(opaque));
    return 0;
}

static const VMStateDescription vmstate_mcxn_mrt = {
    .name = TYPE_MCXN_MRT,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mcxn_mrt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, MCXNMRTState),
        VMSTATE_UINT32_ARRAY(intval, MCXNMRTState, MCXN_MRT_CHANNELS),
        VMSTATE_UINT32_ARRAY(ctrl, MCXNMRTState, MCXN_MRT_CHANNELS),
        VMSTATE_UINT32_ARRAY(stat, MCXNMRTState, MCXN_MRT_CHANNELS),
        VMSTATE_UINT32_ARRAY(load, MCXNMRTState, MCXN_MRT_CHANNELS),
        VMSTATE_INT64_ARRAY(base_ns, MCXNMRTState, MCXN_MRT_CHANNELS),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_mrt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_mrt_realize;
    device_class_set_legacy_reset(dc, mcxn_mrt_reset);
    dc->vmsd = &vmstate_mcxn_mrt;
}

static const TypeInfo mcxn_mrt_types[] = {
    {
        .name          = TYPE_MCXN_MRT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNMRTState),
        .instance_init = mcxn_mrt_init,
        .class_init    = mcxn_mrt_class_init,
    },
};

DEFINE_TYPES(mcxn_mrt_types)
