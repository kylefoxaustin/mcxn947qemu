/*
 * NXP MCX N eFlexPWM (PWM) — bring-up model.  See header for the modelled
 * semantics.  One QOM type ("mcxn-pwm") instantiated for PWM0 and PWM1.
 *
 * Register layout from the MCXN947 CMSIS header (PWM_Type).  Offsets/bit masks
 * are taken verbatim from that header; reset values default to zero (the RM
 * gives 0x0000 reset for the registers exercised at bring-up).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/misc/mcxn_pwm.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Per-submodule register block: base 0x0, step 0x60, four submodules. */
#define PWM_SM_STEP        0x60
#define PWM_SM_COUNT       4
#define PWM_SM_BASE(n)     ((n) * PWM_SM_STEP)

/* Submodule register offsets within a block. */
#define PWM_SM_INIT        0x02    /* Initial count */
#define PWM_SM_VAL1        0x0E    /* Modulo (period) value */
#define PWM_SM_STS         0x24    /* Status (W1C flags) */
#define PWM_SM_INTEN       0x26    /* Interrupt enable */

/* STS / INTEN interrupt bits (submodule). */
#define PWM_STS_CMPF       0x003Fu /* compare flags */
#define PWM_STS_RF         0x1000u /* reload flag */
#define PWM_INTEN_CMPIE    0x003Fu /* compare interrupt enables */
#define PWM_INTEN_RIE      0x1000u /* reload interrupt enable */

/* MCTRL.RUN bit for submodule 0 (bits [11:8], one per submodule). */
#define PWM_MCTRL_RUN_SM0  0x0100u

/* Nominal PWM counter clock: 10 ns/tick (~100 MHz) for the modelled period. */
#define PWM_TICK_NS        10

/* Top-level (shared) registers. */
#define PWM_OUTEN          0x180
#define PWM_MASK           0x182
#define PWM_SWCOUT         0x184
#define PWM_DTSRCSEL       0x186
#define PWM_MCTRL          0x188   /* Master Control */
#define PWM_MCTRL2         0x18A
#define PWM_FCTRL          0x18C
#define PWM_FSTS           0x18E   /* Fault Status (W1C flags) */
#define PWM_FFILT          0x190
#define PWM_FTST           0x192
#define PWM_FCTRL2         0x194

/* MCTRL fields. */
#define PWM_MCTRL_LDOK_MASK    0x000Fu
#define PWM_MCTRL_CLDOK_MASK   0x00F0u
#define PWM_MCTRL_RUN_MASK     0x0F00u

/* STS W1C flag mask (CMPF/CFX/CFB/CFA/RF/REF/RUF). */
#define PWM_STS_W1C_MASK       0x7FFFu
/* FSTS lower W1C fault flags (FFLAG[3:0]). */
#define PWM_FSTS_W1C_MASK      0x000Fu

static inline uint32_t pwm_ld16(MCXNPWMState *s, hwaddr off)
{
    return s->regs[off] | ((uint32_t)s->regs[off + 1] << 8);
}

static inline void pwm_st16(MCXNPWMState *s, hwaddr off, uint16_t v)
{
    s->regs[off] = v & 0xff;
    s->regs[off + 1] = (v >> 8) & 0xff;
}

/* Submodule-0 reload/compare interrupt: (STS & INTEN) on the IRQ-bearing bits. */
static void mcxn_pwm_update_irq(MCXNPWMState *s)
{
    uint16_t sts = pwm_ld16(s, PWM_SM_STS);
    uint16_t inten = pwm_ld16(s, PWM_SM_INTEN);
    bool active = (sts & inten & (PWM_STS_RF | PWM_STS_CMPF)) != 0;

    qemu_set_irq(s->irq, active);
}

/* Submodule-0 counter period from INIT/VAL1 at the nominal PWM tick rate. */
static int64_t mcxn_pwm_period_ns(MCXNPWMState *s)
{
    uint16_t init = pwm_ld16(s, PWM_SM_INIT);
    uint16_t val1 = pwm_ld16(s, PWM_SM_VAL1);
    int64_t span = (uint16_t)(val1 - init) + 1;   /* counter range, wraps ok */
    int64_t ns = span * PWM_TICK_NS;

    return ns < 1000 ? 1000 : ns;                 /* floor to keep it sane */
}

/* One submodule-0 reload: set the reload flag and re-arm. */
static void mcxn_pwm_reload_tick(void *opaque)
{
    MCXNPWMState *s = opaque;

    pwm_st16(s, PWM_SM_STS, pwm_ld16(s, PWM_SM_STS) | PWM_STS_RF);
    mcxn_pwm_update_irq(s);
    timer_mod(&s->reload_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + mcxn_pwm_period_ns(s));
}

static uint64_t mcxn_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNPWMState *s = MCXN_PWM(opaque);
    uint64_t val = 0;
    unsigned i;

    /* Assemble the requested width from the byte-addressable backing. */
    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_PWM_SIZE) {
            val |= (uint64_t)s->regs[offset + i] << (8 * i);
        }
    }
    return val;
}

static void mcxn_pwm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNPWMState *s = MCXN_PWM(opaque);
    unsigned i;
    int n;

    /* Special-case the registers with side effects.  These are all 16-bit and
     * naturally addressed; for sub-register writes fall through to the plain
     * byte store below. */
    if (size == 2 && offset == PWM_MCTRL) {
        uint16_t mctrl = value & 0xffff;
        uint16_t run = mctrl & PWM_MCTRL_RUN_MASK;
        /* RUN bits reflect back so firmware sees the PWM running.  LDOK is a
         * "load OK" request that hardware clears once the buffered registers
         * are loaded; model the load as instantaneous so LDOK reads back 0.
         * CLDOK ("clear LDOK") forces the corresponding LDOK bits to 0. */
        uint16_t cldok = (mctrl & PWM_MCTRL_CLDOK_MASK) >> 4;
        (void)cldok;   /* load is instantaneous, so LDOK is already cleared */
        pwm_st16(s, PWM_MCTRL, run);
        /* Submodule-0 RUN gates the periodic reload timer. */
        if (run & PWM_MCTRL_RUN_SM0) {
            timer_mod(&s->reload_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                        mcxn_pwm_period_ns(s));
        } else {
            timer_del(&s->reload_timer);
        }
        return;
    }

    if (size == 2 && offset < PWM_SM_BASE(PWM_SM_COUNT)) {
        /* Submodule status register: write-1-to-clear. */
        for (n = 0; n < PWM_SM_COUNT; n++) {
            hwaddr sts = PWM_SM_BASE(n) + PWM_SM_STS;
            if (offset == sts) {
                uint16_t cur = pwm_ld16(s, sts);
                cur &= ~((uint16_t)value & PWM_STS_W1C_MASK);
                pwm_st16(s, sts, cur);
                if (n == 0) {
                    mcxn_pwm_update_irq(s);
                }
                return;
            }
        }
    }

    if (size == 2 && offset == PWM_FSTS) {
        uint16_t cur = pwm_ld16(s, PWM_FSTS);
        cur &= ~((uint16_t)value & PWM_FSTS_W1C_MASK);
        pwm_st16(s, PWM_FSTS, cur);
        return;
    }

    /* Generic byte-addressable store. */
    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_PWM_SIZE) {
            s->regs[offset + i] = (value >> (8 * i)) & 0xff;
        }
    }

    /* A write touching submodule-0 INTEN can change the interrupt condition. */
    if (offset <= PWM_SM_INTEN + 1 && offset + size > PWM_SM_INTEN) {
        mcxn_pwm_update_irq(s);
    }
}

static const MemoryRegionOps mcxn_pwm_ops = {
    .read = mcxn_pwm_read,
    .write = mcxn_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_pwm_reset(DeviceState *dev)
{
    MCXNPWMState *s = MCXN_PWM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    timer_del(&s->reload_timer);
    qemu_set_irq(s->irq, 0);
}

static void mcxn_pwm_realize(DeviceState *dev, Error **errp)
{
    MCXNPWMState *s = MCXN_PWM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_pwm_ops, s,
                          TYPE_MCXN_PWM, MCXN_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    timer_init_ns(&s->reload_timer, QEMU_CLOCK_VIRTUAL, mcxn_pwm_reload_tick, s);
}

static const VMStateDescription vmstate_mcxn_pwm = {
    .name = TYPE_MCXN_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MCXNPWMState, MCXN_PWM_SIZE),
        VMSTATE_TIMER(reload_timer, MCXNPWMState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_pwm_realize;
    device_class_set_legacy_reset(dc, mcxn_pwm_reset);
    dc->vmsd = &vmstate_mcxn_pwm;
}

static const TypeInfo mcxn_pwm_types[] = {
    {
        .name          = TYPE_MCXN_PWM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPWMState),
        .class_init    = mcxn_pwm_class_init,
    },
};

DEFINE_TYPES(mcxn_pwm_types)
