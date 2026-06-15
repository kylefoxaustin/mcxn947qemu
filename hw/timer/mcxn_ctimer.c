/*
 * NXP MCX N CTIMER (Standard counter/timer) — functional core.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/timer/mcxn_ctimer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS CTIMER_Type). */
#define R_IR    0x00
#define R_TCR   0x04
#define R_TC    0x08
#define R_PR    0x0C
#define R_PC    0x10
#define R_MCR   0x14
#define R_MR0   0x18    /* MR0..3  @0x18..0x24 */
#define R_CCR   0x28
#define R_CR0   0x2C    /* CR0..3  @0x2C..0x38 */
#define R_EMR   0x3C
#define R_CTCR  0x70
#define R_PWMC  0x74
#define R_MSR0  0x78    /* MSR0..3 @0x78..0x84 */

#define TCR_CEN   (1u << 0)
#define TCR_CRST  (1u << 1)

/* MCR packs 3 bits per match: INT (interrupt), RST (reset TC), STOP. */
#define MCR_INT(n)   (1u << (3 * (n) + 0))
#define MCR_RST(n)   (1u << (3 * (n) + 1))
#define MCR_STOP(n)  (1u << (3 * (n) + 2))
#define MCR_ANY(n)   (MCR_INT(n) | MCR_RST(n) | MCR_STOP(n))

static bool ctimer_running(MCXNCTimerState *s)
{
    return (s->tcr & TCR_CEN) && !(s->tcr & TCR_CRST);
}

static uint32_t ctimer_freq(MCXNCTimerState *s)
{
    uint32_t hz = s->clk ? clock_get_hz(s->clk) : 0;
    return hz ? hz : 150000000;   /* fallback if the clock tree isn't driven */
}

/* Live tc/pc at time `now`, without committing to state. */
static void ctimer_peek(MCXNCTimerState *s, int64_t now,
                        uint32_t *tc, uint32_t *pc)
{
    uint64_t elapsed, pclk, total;
    uint32_t div;

    *tc = s->tc;
    *pc = s->pc;
    if (!ctimer_running(s) || now <= s->base_ns) {
        return;
    }
    div = s->pr + 1;
    elapsed = now - s->base_ns;
    pclk = (elapsed * ctimer_freq(s)) / 1000000000ULL;
    total = (uint64_t)s->pc + pclk;
    *tc = s->tc + (uint32_t)(total / div);
    *pc = (uint32_t)(total % div);
}

static void ctimer_sync(MCXNCTimerState *s, int64_t now)
{
    ctimer_peek(s, now, &s->tc, &s->pc);
    s->base_ns = now;
}

static void ctimer_update_irq(MCXNCTimerState *s)
{
    qemu_set_irq(s->irq, s->ir != 0);
}

/* Arm the host timer for the soonest match that has an action. */
static void ctimer_reschedule(MCXNCTimerState *s, int64_t now)
{
    uint32_t div = s->pr + 1;
    uint32_t freq = ctimer_freq(s);
    uint64_t best = UINT64_MAX;
    int n;

    timer_del(&s->timer);
    if (!ctimer_running(s)) {
        return;
    }
    for (n = 0; n < 4; n++) {
        uint64_t dtc, clocks, ns;

        if (!(s->mcr & MCR_ANY(n))) {
            continue;
        }
        dtc = (s->mr[n] >= s->tc) ? (s->mr[n] - s->tc)
                                  : ((1ULL << 32) - s->tc + s->mr[n]);
        clocks = dtc * div;
        clocks = (clocks > s->pc) ? clocks - s->pc : 1;
        ns = (clocks * 1000000000ULL) / freq;
        if (ns < best) {
            best = ns;
        }
    }
    if (best != UINT64_MAX) {
        timer_mod(&s->timer, now + (int64_t)(best ? best : 1));
    }
}

static void ctimer_tick(void *opaque)
{
    MCXNCTimerState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t best_dtc = UINT64_MAX;
    int n, fire = -1;

    ctimer_sync(s, now);

    /* The soonest active match is the one firing now; snap TC to it exactly to
     * avoid sub-tick rounding error. */
    for (n = 0; n < 4; n++) {
        uint64_t dtc;

        if (!(s->mcr & MCR_ANY(n))) {
            continue;
        }
        dtc = (s->mr[n] >= s->tc) ? (s->mr[n] - s->tc)
                                  : ((1ULL << 32) - s->tc + s->mr[n]);
        if (dtc < best_dtc) {
            best_dtc = dtc;
            fire = n;
        }
    }
    if (fire >= 0) {
        bool do_reset = false;

        s->tc = s->mr[fire];
        s->pc = 0;
        for (n = 0; n < 4; n++) {            /* all matches at this TC value */
            if ((s->mcr & MCR_ANY(n)) && s->mr[n] == s->tc) {
                if (s->mcr & MCR_INT(n)) {
                    s->ir |= (1u << n);
                }
                if (s->mcr & MCR_STOP(n)) {
                    s->tcr &= ~TCR_CEN;
                }
                if (s->mcr & MCR_RST(n)) {
                    do_reset = true;
                }
            }
        }
        if (do_reset) {
            s->tc = 0;
            s->pc = 0;
        }
        s->base_ns = now;
    }
    ctimer_update_irq(s);
    ctimer_reschedule(s, now);
}

static uint64_t ctimer_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNCTimerState *s = MCXN_CTIMER(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t tc, pc;

    switch (off) {
    case R_IR:   return s->ir;
    case R_TCR:  return s->tcr;
    case R_TC:   ctimer_peek(s, now, &tc, &pc); return tc;
    case R_PR:   return s->pr;
    case R_PC:   ctimer_peek(s, now, &tc, &pc); return pc;
    case R_MCR:  return s->mcr;
    case R_MR0: case R_MR0 + 4: case R_MR0 + 8: case R_MR0 + 12:
        return s->mr[(off - R_MR0) / 4];
    case R_CCR:  return s->ccr;
    case R_CR0: case R_CR0 + 4: case R_CR0 + 8: case R_CR0 + 12:
        return s->cr[(off - R_CR0) / 4];
    case R_EMR:  return s->emr;
    case R_CTCR: return s->ctcr;
    case R_PWMC: return s->pwmc;
    case R_MSR0: case R_MSR0 + 4: case R_MSR0 + 8: case R_MSR0 + 12:
        return s->msr[(off - R_MSR0) / 4];
    default:
        return 0;
    }
}

static void ctimer_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MCXNCTimerState *s = MCXN_CTIMER(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t v = val;

    switch (off) {
    case R_IR:                         /* write-1-to-clear */
        s->ir &= ~v;
        ctimer_update_irq(s);
        return;
    case R_TCR:
        ctimer_sync(s, now);
        s->tcr = v & 0xFF;
        if (s->tcr & TCR_CRST) {
            s->tc = 0;
            s->pc = 0;
        }
        ctimer_reschedule(s, now);
        return;
    case R_TC:  ctimer_sync(s, now); s->tc = v; ctimer_reschedule(s, now); return;
    case R_PR:  ctimer_sync(s, now); s->pr = v; ctimer_reschedule(s, now); return;
    case R_PC:  ctimer_sync(s, now); s->pc = v; ctimer_reschedule(s, now); return;
    case R_MCR: ctimer_sync(s, now); s->mcr = v; ctimer_reschedule(s, now); return;
    case R_MR0: case R_MR0 + 4: case R_MR0 + 8: case R_MR0 + 12:
        ctimer_sync(s, now);
        s->mr[(off - R_MR0) / 4] = v;
        ctimer_reschedule(s, now);
        return;
    case R_CCR:  s->ccr = v; return;
    case R_CR0: case R_CR0 + 4: case R_CR0 + 8: case R_CR0 + 12:
        return;                        /* capture registers are read-only */
    case R_EMR:  s->emr = v; return;
    case R_CTCR: s->ctcr = v; return;
    case R_PWMC: s->pwmc = v; return;
    case R_MSR0: case R_MSR0 + 4: case R_MSR0 + 8: case R_MSR0 + 12:
        s->msr[(off - R_MSR0) / 4] = v;
        return;
    default:
        return;
    }
}

static const MemoryRegionOps ctimer_ops = {
    .read = ctimer_read,
    .write = ctimer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_ctimer_reset(DeviceState *dev)
{
    MCXNCTimerState *s = MCXN_CTIMER(dev);

    timer_del(&s->timer);
    s->ir = s->tcr = s->pr = s->mcr = 0;
    s->ccr = s->emr = s->ctcr = s->pwmc = 0;
    memset(s->mr, 0, sizeof(s->mr));
    memset(s->cr, 0, sizeof(s->cr));
    memset(s->msr, 0, sizeof(s->msr));
    s->tc = s->pc = 0;
    s->base_ns = 0;
}

static void mcxn_ctimer_init(Object *obj)
{
    MCXNCTimerState *s = MCXN_CTIMER(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static void mcxn_ctimer_realize(DeviceState *dev, Error **errp)
{
    MCXNCTimerState *s = MCXN_CTIMER(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &ctimer_ops, s,
                          TYPE_MCXN_CTIMER, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, ctimer_tick, s);
}

static const VMStateDescription vmstate_mcxn_ctimer = {
    .name = TYPE_MCXN_CTIMER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, MCXNCTimerState),
        VMSTATE_UINT32(ir, MCXNCTimerState),
        VMSTATE_UINT32(tcr, MCXNCTimerState),
        VMSTATE_UINT32(pr, MCXNCTimerState),
        VMSTATE_UINT32(mcr, MCXNCTimerState),
        VMSTATE_UINT32_ARRAY(mr, MCXNCTimerState, 4),
        VMSTATE_UINT32(ccr, MCXNCTimerState),
        VMSTATE_UINT32(emr, MCXNCTimerState),
        VMSTATE_UINT32(ctcr, MCXNCTimerState),
        VMSTATE_UINT32(pwmc, MCXNCTimerState),
        VMSTATE_UINT32_ARRAY(cr, MCXNCTimerState, 4),
        VMSTATE_UINT32_ARRAY(msr, MCXNCTimerState, 4),
        VMSTATE_UINT32(tc, MCXNCTimerState),
        VMSTATE_UINT32(pc, MCXNCTimerState),
        VMSTATE_INT64(base_ns, MCXNCTimerState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_ctimer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_ctimer_realize;
    device_class_set_legacy_reset(dc, mcxn_ctimer_reset);
    dc->vmsd = &vmstate_mcxn_ctimer;
}

static const TypeInfo mcxn_ctimer_types[] = {
    {
        .name          = TYPE_MCXN_CTIMER,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCTimerState),
        .instance_init = mcxn_ctimer_init,
        .class_init    = mcxn_ctimer_class_init,
    },
};

DEFINE_TYPES(mcxn_ctimer_types)
