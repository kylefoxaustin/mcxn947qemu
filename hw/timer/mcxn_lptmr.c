/*
 * NXP MCX N LPTMR (Low-Power Timer) — functional model.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/timer/mcxn_lptmr.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"

#define R_CSR 0x0
#define R_PSR 0x4
#define R_CMR 0x8
#define R_CNR 0xC

#define CSR_TEN (1u << 0)
#define CSR_TFC (1u << 2)
#define CSR_TIE (1u << 6)
#define CSR_TCF (1u << 7)

#define PSR_PCS      (0x3u << 0)     /* prescaler/glitch-filter clock select */
#define PSR_PBYP     (1u << 2)
#define PSR_PRESCALE (0xFu << 3)

/*
 * PSR[PCS] selects the LPTMR's functional clock (RM rev 7, Table 463 "LPTMRn
 * prescaler/glitch filter clocking options") -- a LOW-POWER clock, NOT the bus clock:
 *   00 FRO_12M (12 MHz)   01 FRO_16K (16 kHz)   10 32K_CLK (32.768 kHz)   11 OSC_SYS
 * The LPTMR's max is 25 MHz, so a 150 MHz source is impossible on silicon.  FRO_12M
 * comes from the SCG (the "clk" input); FRO_16K and 32K_CLK are fixed low-power
 * oscillators (the same constants SYSCON uses for OSTIMER); OSC_SYS is the external
 * crystal, which has no source in emulation (a board seam) and reports 0.
 */
#define FRO_16K_HZ   16000u          /* fsl_clock.c: CLOCK_GetClk16KFreq()  */
#define CLK_32K_HZ   32768u          /* 32K_CLK (OSC_32K / FRO_32K)         */

static uint32_t lptmr_divider(MCXNLPTMRState *s)
{
    if (s->psr & PSR_PBYP) {
        return 1;
    }
    return 1u << (((s->psr & PSR_PRESCALE) >> 3) + 1);
}

static uint32_t lptmr_source_hz(MCXNLPTMRState *s)
{
    switch (s->psr & PSR_PCS) {
    case 0:
        return s->clk ? clock_get_hz(s->clk) : 0;  /* FRO_12M (from SCG)  */
    case 1:  return FRO_16K_HZ;                          /* FRO_16K            */
    case 2:  return CLK_32K_HZ;                          /* 32K_CLK            */
    default:                                             /* 11 = OSC_SYS       */
        qemu_log_mask(LOG_UNIMP,
            "mcxn-lptmr: PCS=3 selects OSC_SYS (external crystal), which has no source "
            "in emulation -- reporting 0 Hz, so the timer does not run (a board seam, not "
            "a plausible-but-wrong rate).\n");
        return 0;
    }
}

static uint32_t lptmr_freq(MCXNLPTMRState *s)
{
    /* No 150 MHz fallback: a low-power timer never runs on the bus clock, and a source
     * of 0 (OSC_SYS with no crystal, or an ungated FRO_12M) means it does not run. */
    return lptmr_source_hz(s) / lptmr_divider(s);
}

static uint32_t lptmr_peek(MCXNLPTMRState *s, int64_t now)
{
    uint64_t counts;

    if (!(s->csr & CSR_TEN) || now <= s->base_ns) {
        return s->cnr;
    }
    counts = ((uint64_t)(now - s->base_ns) * lptmr_freq(s)) / 1000000000ULL;
    return (uint32_t)(s->cnr + counts) & 0xFFFF;   /* 16-bit counter */
}

static void lptmr_update_irq(MCXNLPTMRState *s)
{
    qemu_set_irq(s->irq, (s->csr & CSR_TCF) && (s->csr & CSR_TIE));
}

static void lptmr_reschedule(MCXNLPTMRState *s, int64_t now)
{
    uint32_t cur, period, remain, freq;

    timer_del(&s->timer);
    if (!(s->csr & CSR_TEN)) {
        return;
    }
    freq = lptmr_freq(s);
    if (freq == 0) {
        return;                          /* no clock source: the timer does not run */
    }
    cur = lptmr_peek(s, now);
    period = s->cmr + 1;                 /* compare fires at CNR == CMR */
    remain = (cur <= s->cmr) ? (s->cmr - cur + 1) : 1;
    (void)period;
    timer_mod(&s->timer,
              now + (int64_t)((uint64_t)remain * 1000000000ULL / freq));
}

static void lptmr_tick(void *opaque)
{
    MCXNLPTMRState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->csr |= CSR_TCF;
    if (!(s->csr & CSR_TFC)) {
        s->cnr = 0;                      /* time-counter mode: restart */
        s->base_ns = now;
    } else {
        s->cnr = lptmr_peek(s, now);
        s->base_ns = now;
    }
    lptmr_update_irq(s);

    /*
     * Pulse the trigger output.  A trigger is an EVENT, not a level: INPUTMUX routes
     * the edge, and the consumer (ADC/DAC) acts on it once.  Holding it high would
     * make a single compare match look like a continuous conversion request.
     */
    qemu_irq_pulse(s->trigger);

    lptmr_reschedule(s, now);
}

static uint64_t lptmr_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNLPTMRState *s = MCXN_LPTMR(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    switch (off) {
    case R_CSR: return s->csr;
    case R_PSR: return s->psr;
    case R_CMR: return s->cmr;
    case R_CNR: return lptmr_peek(s, now);
    default:    return 0;
    }
}

static void lptmr_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MCXNLPTMRState *s = MCXN_LPTMR(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t v = val;

    switch (off) {
    case R_CSR: {
        bool was_en = s->csr & CSR_TEN;

        if (v & CSR_TCF) {              /* TCF is write-1-to-clear */
            s->csr &= ~CSR_TCF;
        }
        s->csr = (s->csr & CSR_TCF) | (v & ~CSR_TCF);
        if ((s->csr & CSR_TEN) && !was_en) {
            s->cnr = 0;
            s->base_ns = now;
        }
        if (!(s->csr & CSR_TEN)) {
            s->cnr = 0;
        }
        lptmr_update_irq(s);
        lptmr_reschedule(s, now);
        return;
    }
    case R_PSR:
        s->psr = v;
        lptmr_reschedule(s, now);
        return;
    case R_CMR:
        s->cmr = v & 0xFFFF;
        lptmr_reschedule(s, now);
        return;
    case R_CNR:
        return;                /* writing CNR latches on HW; ignore */
    default:
        return;
    }
}

static const MemoryRegionOps lptmr_ops = {
    .read = lptmr_read,
    .write = lptmr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_lptmr_reset(DeviceState *dev)
{
    MCXNLPTMRState *s = MCXN_LPTMR(dev);

    timer_del(&s->timer);
    s->csr = s->psr = s->cmr = s->cnr = 0;
    s->base_ns = 0;
}

static void mcxn_lptmr_init(Object *obj)
{
    MCXNLPTMRState *s = MCXN_LPTMR(obj);

    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static void mcxn_lptmr_realize(DeviceState *dev, Error **errp)
{
    MCXNLPTMRState *s = MCXN_LPTMR(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &lptmr_ops, s,
                          TYPE_MCXN_LPTMR, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    qdev_init_gpio_out_named(dev, &s->trigger, "trigger", 1);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, lptmr_tick, s);
}

static const VMStateDescription vmstate_mcxn_lptmr = {
    .name = TYPE_MCXN_LPTMR,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER(timer, MCXNLPTMRState),
        VMSTATE_UINT32(csr, MCXNLPTMRState),
        VMSTATE_UINT32(psr, MCXNLPTMRState),
        VMSTATE_UINT32(cmr, MCXNLPTMRState),
        VMSTATE_UINT32(cnr, MCXNLPTMRState),
        VMSTATE_INT64(base_ns, MCXNLPTMRState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_lptmr_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_lptmr_realize;
    device_class_set_legacy_reset(dc, mcxn_lptmr_reset);
    dc->vmsd = &vmstate_mcxn_lptmr;
}

static const TypeInfo mcxn_lptmr_types[] = {
    {
        .name          = TYPE_MCXN_LPTMR,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNLPTMRState),
        .instance_init = mcxn_lptmr_init,
        .class_init    = mcxn_lptmr_class_init,
    },
};

DEFINE_TYPES(mcxn_lptmr_types)
