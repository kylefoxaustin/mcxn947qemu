/*
 * NXP MCX N OSTIMER (OS Event Timer) — functional model.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/timer/mcxn_ostimer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"

#define R_EVTIMERL    0x00   /* RO, gray-coded counter low  */
#define R_EVTIMERH    0x04   /* RO, gray-coded counter high */
#define R_CAPTURE_L   0x08
#define R_CAPTURE_H   0x0C
#define R_MATCH_L     0x10
#define R_MATCH_H     0x14
#define R_OSEVENT_CTRL 0x1C

#define CTRL_INTRFLAG (1u << 0)   /* write-1-to-clear */
#define CTRL_INTENA   (1u << 1)

/* The OSTIMER runs on the 1 MHz clk_1m by default. */
#define OSTIMER_HZ 1000000u

static uint64_t bin_to_gray(uint64_t n)
{
    return n ^ (n >> 1);
}

static uint64_t gray_to_bin(uint64_t g)
{
    uint64_t b = g;
    while (g >>= 1) {
        b ^= g;
    }
    return b;
}

static uint32_t ostimer_freq(MCXNOSTimerState *s)
{
    uint32_t hz = s->clk ? clock_get_hz(s->clk) : 0;
    return hz ? hz : OSTIMER_HZ;
}

static uint64_t ostimer_count(MCXNOSTimerState *s, int64_t now)
{
    uint64_t elapsed = (now > s->base_ns) ? (now - s->base_ns) : 0;
    return s->base_count + ((elapsed * ostimer_freq(s)) / 1000000000ULL);
}

static void ostimer_update_irq(MCXNOSTimerState *s)
{
    qemu_set_irq(s->irq, (s->ctrl & CTRL_INTRFLAG) && (s->ctrl & CTRL_INTENA));
}

static void ostimer_reschedule(MCXNOSTimerState *s, int64_t now)
{
    uint64_t cur = ostimer_count(s, now);

    timer_del(&s->timer);
    if (!(s->ctrl & CTRL_INTENA) || s->match <= cur) {
        return;
    }
    timer_mod(&s->timer, now + (int64_t)(((s->match - cur) * 1000000000ULL)
                                         / ostimer_freq(s)));
}

static void ostimer_tick(void *opaque)
{
    MCXNOSTimerState *s = opaque;

    s->ctrl |= CTRL_INTRFLAG;
    ostimer_update_irq(s);
}

static uint64_t mcxn_ostimer_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNOSTimerState *s = MCXN_OSTIMER(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t gray;

    switch (off) {
    case R_EVTIMERL:  return bin_to_gray(ostimer_count(s, now)) & 0xFFFFFFFF;
    case R_EVTIMERH:  return (bin_to_gray(ostimer_count(s, now)) >> 32) & 0x3FF;
    case R_CAPTURE_L: return s->capture_l;
    case R_CAPTURE_H: return s->capture_h;
    case R_MATCH_L:   gray = bin_to_gray(s->match); return gray & 0xFFFFFFFF;
    case R_MATCH_H:   gray = bin_to_gray(s->match); return (gray >> 32) & 0x3FF;
    case R_OSEVENT_CTRL: return s->ctrl;
    default: return 0;
    }
}

static void mcxn_ostimer_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
{
    MCXNOSTimerState *s = MCXN_OSTIMER(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t v = val;

    switch (off) {
    case R_MATCH_L:
        s->match_gray_l = v;
        s->match = gray_to_bin(((uint64_t)s->match_gray_h << 32) |
                               s->match_gray_l);
        ostimer_reschedule(s, now);
        return;
    case R_MATCH_H:
        s->match_gray_h = v & 0x3FF;
        s->match = gray_to_bin(((uint64_t)s->match_gray_h << 32) |
                               s->match_gray_l);
        ostimer_reschedule(s, now);
        return;
    case R_OSEVENT_CTRL:
        if (v & CTRL_INTRFLAG) {            /* write-1-to-clear */
            s->ctrl &= ~CTRL_INTRFLAG;
        }
        s->ctrl = (s->ctrl & CTRL_INTRFLAG) | (v & CTRL_INTENA);
        ostimer_update_irq(s);
        ostimer_reschedule(s, now);
        return;
    default:
        return;
    }
}

static const MemoryRegionOps mcxn_ostimer_ops = {
    .read = mcxn_ostimer_read,
    .write = mcxn_ostimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_ostimer_reset(DeviceState *dev)
{
    MCXNOSTimerState *s = MCXN_OSTIMER(dev);

    timer_del(&s->timer);
    s->base_count = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->match = 0;
    s->match_gray_l = s->match_gray_h = 0;
    s->ctrl = 0;
    s->capture_l = s->capture_h = 0;
}

static void mcxn_ostimer_init(Object *obj)
{
    MCXNOSTimerState *s = MCXN_OSTIMER(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static void mcxn_ostimer_realize(DeviceState *dev, Error **errp)
{
    MCXNOSTimerState *s = MCXN_OSTIMER(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_ostimer_ops, s,
                          TYPE_MCXN_OSTIMER, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, ostimer_tick, s);
}

static const VMStateDescription vmstate_mcxn_ostimer = {
    .name = TYPE_MCXN_OSTIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, MCXNOSTimerState),
        VMSTATE_UINT64(base_count, MCXNOSTimerState),
        VMSTATE_INT64(base_ns, MCXNOSTimerState),
        VMSTATE_UINT64(match, MCXNOSTimerState),
        VMSTATE_UINT32(match_gray_l, MCXNOSTimerState),
        VMSTATE_UINT32(match_gray_h, MCXNOSTimerState),
        VMSTATE_UINT32(ctrl, MCXNOSTimerState),
        VMSTATE_UINT32(capture_l, MCXNOSTimerState),
        VMSTATE_UINT32(capture_h, MCXNOSTimerState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_ostimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_ostimer_realize;
    device_class_set_legacy_reset(dc, mcxn_ostimer_reset);
    dc->vmsd = &vmstate_mcxn_ostimer;
}

static const TypeInfo mcxn_ostimer_types[] = {
    {
        .name          = TYPE_MCXN_OSTIMER,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNOSTimerState),
        .instance_init = mcxn_ostimer_init,
        .class_init    = mcxn_ostimer_class_init,
    },
};

DEFINE_TYPES(mcxn_ostimer_types)
