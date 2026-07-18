/*
 * NXP MCX N SCT (SCTimer/PWM) — bring-up model.  See header for the modelled
 * semantics.  Single instance ("mcxn-sct") for SCT0.
 *
 * Register layout from the MCXN947 CMSIS header (SCT_Type).  Offsets/bit masks
 * are taken verbatim from that header; reset values default to zero (the RM
 * gives 0x00000000 reset for the registers exercised at bring-up).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/misc/mcxn_sct.h"
#include "hw/core/irq.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS SCT_Type). */
#define SCT_CONFIG   0x00
#define SCT_CTRL     0x04
#define SCT_COUNT    0x40
#define SCT_EVEN     0xF0    /* Event Interrupt Enable */
#define SCT_EVFLAG   0xF4    /* Event Flag (W1C) */
#define SCT_CONFLAG  0xFC    /* Conflict Flag (W1C) */
#define SCT_MATCHREL0 0x180  /* Match Reload value 0 (the limit) */
#define SCT_DMAREQ0  0x5C    /* DMA Request 0: DEV_n selects events -> DMA req 0 */
#define SCT_DMAREQ1  0x60    /* DMA Request 1: DEV_n selects events -> DMA req 1 */

/* DMAREQ0/1[DEV_0]: does event 0 (the modelled match/limit event) drive this request? */
#define SCT_DMAREQ_DEV0  (1u << 0)

/* CTRL self-clearing counter-clear bits (low and high 16-bit counter halves). */
#define SCT_CTRL_CLRCTR_L_MASK   0x00000008u
#define SCT_CTRL_CLRCTR_H_MASK   0x00080000u
/* CTRL run-gating bits for the low counter. */
#define SCT_CTRL_STOP_L_MASK     0x00000002u
#define SCT_CTRL_HALT_L_MASK     0x00000004u

/* Event 0 (the modelled match/limit event) in EVFLAG/EVEN. */
#define SCT_EV0   (1u << 0)

/*
 * The SCT counter clock.
 *
 * ⚠ THIS USED TO BE AN INVENTED 10 ns/tick ("nominal ~100 MHz") -- a number that
 * appears NOWHERE in the RM.  It is the IDENTICAL bug already fixed in the eFlexPWM
 * (hw/misc/mcxn_pwm.c), and I fixed it THERE and left it sitting HERE.
 *
 *     ⭐ A FIX APPLIED IN ONE PLACE IS NOT A FIX.  The PWM's own comment says "THIS
 *        USED TO BE AN INVENTED 10 ns/tick" and the sibling file still had the
 *        constant, verbatim, with the same word ("nominal") standing over it.
 *
 * And the word is the tell.  "nominal", "plausible", "reasonable", "best-effort",
 * "approximate" -- THESE ARE THE WORDS YOU USE WHEN YOU MEAN FABRICATED.  Grepping
 * the tree for that vocabulary is what found this.
 *
 * WORSE: THE PRESCALER WAS NOT MODELLED AT ALL.  CTRL[PRE_L] (bits 12:5, CMSIS
 * SCT_CTRL_PRE_L_MASK = 0x1FE0) divides the counter clock by PRE_L+1, and this model
 * ignored it completely -- so a driver asking for a 256x slower count got EXACTLY THE
 * SAME RATE, silently.  That is the same collapsed oracle that made the eFlexPWM
 * carrier "verified" while its prescaler did not exist: a test that never sweeps the
 * axis cannot see that the axis is not wired up.
 *
 * Per the RM the counter is clocked from the SCT clock (SYSCON SCTCLKSEL/SCTCLKDIV)
 * divided by CTRL[PRE_L]+1.  The clock tree is NOT MODELLED, so the input rate is a
 * DOCUMENTED ASSUMPTION tied to the SoC system clock (mcxn_frdm.c drives sysclk at
 * 150 MHz) rather than a free-floating constant.  The PRESCALER RATIO IS EXACT; the
 * absolute frequency is the assumption.  Say so, don't hide it.
 */
/*
 * ⚠ THIS USED TO BE:  #define SCT_CLOCK_HZ 150000000  -- "= SoC sysclk, a stated
 * assumption" -- and the comment SIX LINES ABOVE IT said, correctly:
 *
 *     "Per the RM the counter is clocked from the SCT clock (SYSCON SCTCLKSEL /
 *      SCTCLKDIV) divided by CTRL[PRE_L]."
 *
 * IT NAMED THE REGISTER THAT DECIDES THE RATE AND THEN IGNORED IT.  Every stock NXP
 * example does CLOCK_AttachClk(kFRO_HF_to_SCT) -- selector 3, FRO_HF, 48 MHz -- and we
 * ticked the SCT at 150 MHz.  3.1x TOO FAST, silently.
 *
 *     ⭐ AND THE TEST COULD NOT SEE IT, BECAUSE THE TEST TOOK THE SAME ASSUMPTION.
 *        tests/mcxn-sct predicts SysTick ticks as (MATCHREL+1)*(PRE_L+1) -- which is
 *        only true if the SCT clock EQUALS the CPU clock.  That is THIS MODEL'S
 *        CONSTANT, not silicon's.  Model and test were wrong together, and the test
 *        passed.
 *
 *        A MIRROR THAT DECLARES ITSELF IS STILL A MIRROR.  (rt1180emulator shipped the
 *        identical specimen: a PWM golden that said, in its own comment, "if PWM_CLK
 *        were wrong, this golden would be wrong in exactly the same direction AND STILL
 *        PASS."  It was.  It did.)
 *
 * The rate now comes from SYSCON.  There is NO DEFAULT: 0 Hz means no source selected,
 * and an SCT with no clock DOES NOT COUNT.
 */
#define SCT_CTRL_PRE_L_MASK   0x00001FE0u
#define SCT_CTRL_PRE_L_SHIFT  5

static inline uint32_t sct_ld32(MCXNSCTState *s, hwaddr off)
{
    return s->regs[off] |
           ((uint32_t)s->regs[off + 1] << 8) |
           ((uint32_t)s->regs[off + 2] << 16) |
           ((uint32_t)s->regs[off + 3] << 24);
}

static inline void sct_st32(MCXNSCTState *s, hwaddr off, uint32_t v)
{
    s->regs[off] = v & 0xff;
    s->regs[off + 1] = (v >> 8) & 0xff;
    s->regs[off + 2] = (v >> 16) & 0xff;
    s->regs[off + 3] = (v >> 24) & 0xff;
}

static void mcxn_sct_update_irq(MCXNSCTState *s)
{
    bool active = (sct_ld32(s, SCT_EVFLAG) & sct_ld32(s, SCT_EVEN)) != 0;
    qemu_set_irq(s->irq, active);
}

static int64_t mcxn_sct_period_ns(MCXNSCTState *s)
{
    int64_t limit = (int64_t)sct_ld32(s, SCT_MATCHREL0) + 1;
    uint32_t ctrl = sct_ld32(s, SCT_CTRL);
    /* CTRL[PRE_L] divides the counter clock by PRE_L + 1. */
    int64_t pre = ((ctrl & SCT_CTRL_PRE_L_MASK) >> SCT_CTRL_PRE_L_SHIFT) + 1;
    int64_t ticks = limit * pre;
    uint32_t hz = s->clk ? clock_get_hz(s->clk) : 0;
    int64_t ns;

    if (!hz) {
        return 0;                       /* no clock selected: the SCT does not count */
    }
    ns = ticks * 1000000000LL / hz;
    return ns < 1 ? 1 : ns;
}

/* One match/limit event: set event-0 flag and re-arm. */
static void mcxn_sct_event_tick(void *opaque)
{
    MCXNSCTState *s = opaque;

    sct_st32(s, SCT_EVFLAG, sct_ld32(s, SCT_EVFLAG) | SCT_EV0);
    mcxn_sct_update_irq(s);
    /*
     * Event 0 can drive either eDMA request line (CMSIS SCT0 DMA0=19, DMA1=20).
     * DMAREQ0/1[DEV_0] selects whether this event feeds each one; the event itself
     * is the trigger, as a one-shot PULSE (one event, one minor loop -- serviced by
     * the eDMA edge path).  INPUTMUX still gates the line downstream. */
    if (sct_ld32(s, SCT_DMAREQ0) & SCT_DMAREQ_DEV0) {
        qemu_irq_pulse(s->dma_req[0]);
    }
    if (sct_ld32(s, SCT_DMAREQ1) & SCT_DMAREQ_DEV0) {
        qemu_irq_pulse(s->dma_req[1]);
    }
    /* Re-arm from the previous DEADLINE, never from "now": re-adding the
     * callback's dispatch latency every period makes the error accumulate, so the
     * event rate runs systematically slow and drifts without bound. */
    {
        int64_t period = mcxn_sct_period_ns(s);

        if (period <= 0) {
            return;      /* no clock selected -- the counter has stopped */
        }
        s->next_event_ns += period;
        timer_mod(&s->event_timer, s->next_event_ns);
    }
}

static uint64_t mcxn_sct_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSCTState *s = MCXN_SCT(opaque);
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_SCT_SIZE) {
            val |= (uint64_t)s->regs[offset + i] << (8 * i);
        }
    }
    return val;
}

static void mcxn_sct_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNSCTState *s = MCXN_SCT(opaque);
    unsigned i;

    /* CTRL: STOP/HALT and the rest of the configuration reflect back, but the
     * CLRCTR_L/CLRCTR_H bits are self-clearing: writing 1 zeroes the matching
     * COUNT half and the bit itself never reads back set. */
    if (size == 4 && offset == SCT_CTRL) {
        uint32_t v = value & 0xffffffffu;
        uint32_t count = sct_ld32(s, SCT_COUNT);

        if (v & SCT_CTRL_CLRCTR_L_MASK) {
            count &= 0xffff0000u;
        }
        if (v & SCT_CTRL_CLRCTR_H_MASK) {
            count &= 0x0000ffffu;
        }
        sct_st32(s, SCT_COUNT, count);
        v &= ~(SCT_CTRL_CLRCTR_L_MASK | SCT_CTRL_CLRCTR_H_MASK);
        sct_st32(s, SCT_CTRL, v);
        /* The low counter runs when neither halted nor stopped. */
        if (!(v & (SCT_CTRL_HALT_L_MASK | SCT_CTRL_STOP_L_MASK))) {
            /* Anchor the first deadline; the callback derives every later one
             * from it, so the event rate cannot drift. */
            int64_t period = mcxn_sct_period_ns(s);

            if (period > 0) {
                s->next_event_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period;
                timer_mod(&s->event_timer, s->next_event_ns);
            } else {
                timer_del(&s->event_timer);   /* no clock: it does not count */
            }
        } else {
            timer_del(&s->event_timer);
        }
        return;
    }

    /* EVFLAG / CONFLAG: write-1-to-clear. */
    if (size == 4 && (offset == SCT_EVFLAG || offset == SCT_CONFLAG)) {
        uint32_t cur = sct_ld32(s, offset);
        cur &= ~(uint32_t)value;
        sct_st32(s, offset, cur);
        if (offset == SCT_EVFLAG) {
            mcxn_sct_update_irq(s);
        }
        return;
    }

    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_SCT_SIZE) {
            s->regs[offset + i] = (value >> (8 * i)) & 0xff;
        }
    }

    /* A write touching EVEN (the interrupt-enable mask) re-evaluates the IRQ. */
    if (offset < SCT_EVEN + 4 && offset + size > SCT_EVEN) {
        mcxn_sct_update_irq(s);
    }
}

static const MemoryRegionOps mcxn_sct_ops = {
    .read = mcxn_sct_read,
    .write = mcxn_sct_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_sct_reset(DeviceState *dev)
{
    MCXNSCTState *s = MCXN_SCT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    timer_del(&s->event_timer);
    qemu_set_irq(s->irq, 0);
}

static void mcxn_sct_realize(DeviceState *dev, Error **errp)
{
    MCXNSCTState *s = MCXN_SCT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_sct_ops, s,
                          TYPE_MCXN_SCT, MCXN_SCT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);          /* 0: NVIC event interrupt */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req[0]);   /* 1: DMA0 request (src 19) */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req[1]);   /* 2: DMA1 request (src 20) */
    timer_init_ns(&s->event_timer, QEMU_CLOCK_VIRTUAL, mcxn_sct_event_tick, s);
}

static const VMStateDescription vmstate_mcxn_sct = {
    .name = TYPE_MCXN_SCT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MCXNSCTState, MCXN_SCT_SIZE),
        VMSTATE_TIMER(event_timer, MCXNSCTState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_sct_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_sct_realize;
    device_class_set_legacy_reset(dc, mcxn_sct_reset);
    dc->vmsd = &vmstate_mcxn_sct;
}

/*
 * ⚠ THE CLOCK INPUT MUST EXIST BEFORE ANYTHING CAN CONNECT TO IT, so it is created in
 * instance_init -- NOT in realize.  Putting it in realize gave:
 *     "Can not find clock-in 'clk' for device type 'mcxn-sct'"
 * because the SoC connects BEFORE realize (qdev_connect_clock_in asserts !realized).
 * Two constraints pointing in opposite directions, one more time.
 */
static void mcxn_sct_init(Object *obj)
{
    MCXNSCTState *s = MCXN_SCT(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static const TypeInfo mcxn_sct_types[] = {
    {
        .name          = TYPE_MCXN_SCT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSCTState),
        .instance_init = mcxn_sct_init,
        .class_init    = mcxn_sct_class_init,
    },
};

DEFINE_TYPES(mcxn_sct_types)
