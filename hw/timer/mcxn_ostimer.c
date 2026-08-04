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
/*
 * ⚠ THERE USED TO BE A `#define OSTIMER_HZ 1000000u` HERE, AND IT IS
 * GONE.
 *
 *   It was the fallback in `return hz ? hz : OSTIMER_HZ;` -- deleted when
 *   the fallback was.  But the CONSTANT survived the fallback,
 *   unreferenced except by the comment that names it, sitting in the
 *   header of the file for the next person to reach for.
 *
 *     ⭐ A DEAD FABRICATION IS STILL AMMUNITION.  An invented number that
 *       nothing reads is one refactor away from being an invented number
 *       that something does.
 *
 *   (Kyle's LAW 1: an unlabelled untestable number is forbidden.  The
 *   cheapest way to label one is to not have it.)
 */

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
    /*
     * ⚠ THIS USED TO BE:  return hz ? hz : 1000000;   (an invented 1 MHz)
     *
     * The Clock input existed and THE SoC NEVER CONNECTED IT, so `hz` was
     * always 0 and the fallback fired every single time.  THE FALLBACK WAS
     * THE CAMOUFLAGE: the model had exactly the right structure and a
     * default that made the missing wiring invisible.  A guest that
     * selected the 16 kHz source (OSTIMERCLKSEL = 0) got a timer running
     * at 1 MHz -- SIXTY-TWO TIMES TOO FAST -- and nothing said a word,
     * because the register that chooses the rate WAS NOT CONSUMED BY
     * ANYTHING.
     *
     * SYSCON now drives this clock from OSTIMERCLKSEL, and there is NO
     * DEFAULT.  0 Hz means NO SOURCE SELECTED (which is the RESET state,
     * OSTIMERCLKSEL = 3) and a timer with no clock DOES NOT RUN.  Silently
     * substituting a plausible rate for a clock nobody turned on is the
     * exact bug this file used to have.
     */
    return s->clk ? clock_get_hz(s->clk) : 0;
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
    case R_MATCH_L:
        gray = bin_to_gray(s->match);
        return gray & 0xFFFFFFFF;
    case R_MATCH_H:
        gray = bin_to_gray(s->match);
        return (gray >> 32) & 0x3FF;
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
    /*
     * ⚠ MATCH RESETS TO ALL-ONES, NOT ZERO.  RM: MATCH_L = 0xFFFF_FFFF.
     *
     *   ⚠ AND I ALMOST OVERCLAIMED THIS.  I was about to write "a match
     *     of zero fires IMMEDIATELY" -- the QDC-period shape.  It does
     *     not: the arm path guards it (`if (!INTENA || s->match <= cur)
     *     return;`), so a zero match simply never arms.  The DANGER was
     *     not real; the WRONG VALUE was.
     *
     *     ⭐ CHECK THE CODE BEFORE YOU CLAIM THE CONSEQUENCE.  A plausible
     *       catastrophe is still a fabrication, and reaching for the
     *       scariest reading of a bug is how you end up with a scary story
     *       instead of a fixed model.
     *
     *   What IS true: the guest read 0 where the silicon reads all-ones,
     *   and MATCH is gray-coded -- so the register must read back the RM's
     *   value, not our binary zero.
     */
    s->match_gray_l = 0xFFFFFFFFu;
    s->match_gray_h = 0x3FFu;
    s->match = gray_to_bin(((uint64_t)s->match_gray_h << 32) | s->match_gray_l);
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
