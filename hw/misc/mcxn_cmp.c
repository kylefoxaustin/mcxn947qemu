/*
 * NXP MCX N CMP (Low-Power Analog Comparator, CMSIS LPCMP_Type) — model.
 *
 * One shared device type instantiated three times on the MCXN947 (CMP0/1/2).
 * The comparator is an analog block; QEMU has no analog stimulus, so the output
 * is OPERATOR-DRIVEN (fidelity-first): rather than hard-wiring CSR[COUT] low,
 * the level the +/- inputs would resolve to is exposed as the runtime QOM
 * property "comparator-output".  Setting it drives CSR[COUT], latches the
 * rising/falling edge flags (CSR[CFR]/CSR[CFF]) and raises the comparator IRQ
 * when the matching IER bit is set — so interrupt- and poll-driven comparator
 * code both progress as on silicon.
 *
 *   - VERID / PARAM are read-only (CMSIS __I) and return constant values.
 *   - CSR[COUT] (bit 8) reflects the operator-set output level.  CFR (bit 0,
 *     rising edge), CFF (bit 1, falling edge) and RRF (bit 2, round-robin) are
 *     write-1-to-clear sticky flags.
 *   - The comparator has no multi-cycle power-up handshake that firmware spins
 *     on, so all control registers are simply stored.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (LPCMP_Type); reset
 * values from the MCX N Reference Manual (section 44.7.1, LPCMP memory map).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_cmp.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define CMP_VERID    0x00   /* RO  Version ID */
#define CMP_PARAM    0x04   /* RO  Parameter */
#define CMP_CCR0     0x08   /* RW  Comparator Control Register 0 */
#define CMP_CCR1     0x0C   /* RW  Comparator Control Register 1 */
#define CMP_CCR2     0x10   /* RW  Comparator Control Register 2 */
#define CMP_DCR      0x18   /* RW  DAC Control */
#define CMP_IER      0x1C   /* RW  Interrupt Enable */
#define CMP_CSR      0x20   /* RW  Comparator Status */
#define CMP_RRCR0    0x24   /* RW  Round Robin Control Register 0 */
#define CMP_RRCR1    0x28   /* RW  Round Robin Control Register 1 */
#define CMP_RRCSR    0x2C   /* RW  Round Robin Control and Status */
#define CMP_RRSR     0x30   /* RW  Round Robin Status */
#define CMP_RRCR2    0x38   /* RW  Round Robin Control Register 2 */

/* CSR fields. */
#define CMP_CSR_CFR     (1u << 0)   /* rising-edge flag, W1C */
#define CMP_CSR_CFF     (1u << 1)   /* falling-edge flag, W1C */
#define CMP_CSR_RRF     (1u << 2)   /* round-robin flag, W1C */
#define CMP_CSR_COUT    (1u << 8)   /* comparator output level, RO */
#define CMP_CSR_W1C_MASK  (CMP_CSR_CFR | CMP_CSR_CFF | CMP_CSR_RRF)

/*
 * IER edge-interrupt enables align 1:1 with the CSR flag bits
 * (CFR_IE/CFF_IE/RRF_IE at 0/1/2).
 */
#define CMP_IER_CFR_IE  (1u << 0)
#define CMP_IER_CFF_IE  (1u << 1)
#define CMP_IER_RRF_IE  (1u << 2)

/* RRCR0 (round-robin control 0). */
#define CMP_RRCR0_RR_EN      (1u << 0)   /* round-robin enable */
/* 0 = external trigger, 1 = internal timer */
#define CMP_RRCR0_RR_TRG_SEL (1u << 1)
/*
 * RRCSR (round-robin control & status): per-channel captured outputs,
 * bit n = channel n.
 */
#define CMP_RRCSR_RR_CH0OUT  (1u << 0)

/*
 * CCR1[DMA_EN] (bit 2): when set, an IER-enabled edge forces a DMA request
 * rather than a CPU interrupt (fsl_lpcmp: LPCMP_EnableDMA -> CCR1[DMA_EN]).
 * The DMA request is a one-shot PULSE serviced through the eDMA edge path.
 */
#define CMP_CCR1_DMA_EN (1u << 2)

/* Constant RO values (RM reset values). */
#define CMP_VERID_VALUE   0x01000041u
#define CMP_PARAM_VALUE   0x00000002u

/* Non-zero RM reset value for a backed control register. */
#define CMP_CCR0_RESET    0x00000002u

/*
 * IER enable bits align 1:1 with the CSR W1C flag bits (CFR_IE/CFF_IE/RRF_IE
 * at 0/1/2, matching CFR/CFF/RRF), so an enabled-and-pending edge is just
 * (CSR & IER & W1C_MASK).
 */
static void mcxn_cmp_update_irq(MCXNCMPState *s)
{
    bool active = (s->regs[CMP_CSR / 4] & s->regs[CMP_IER / 4] &
                   CMP_CSR_W1C_MASK) != 0;

    /*
     * CCR1[DMA_EN] REDIRECTS an enabled edge to the DMA request "rather than
     * a CPU interrupt instead" (RM / fsl_lpcmp): with DMA enabled the
     * comparator does not raise the NVIC line -- the event goes to the eDMA
     * (see mcxn_cmp_set_cout).
     */
    if (s->regs[CMP_CCR1 / 4] & CMP_CCR1_DMA_EN) {
        active = false;
    }
    qemu_set_irq(s->irq, active);
}

static uint64_t mcxn_cmp_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNCMPState *s = MCXN_CMP(opaque);

    switch (offset) {
    case CMP_VERID:
        return CMP_VERID_VALUE;
    case CMP_PARAM:
        return CMP_PARAM_VALUE;
    case CMP_CSR:
        /*
         * COUT reflects the operator-set output level; the edge/round-robin
         * flags hold whatever software has not yet cleared.
         */
        return (s->regs[CMP_CSR / 4] & ~CMP_CSR_COUT) |
               (s->cout ? CMP_CSR_COUT : 0);
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_cmp_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNCMPState *s = MCXN_CMP(opaque);

    switch (offset) {
    case CMP_VERID:
    case CMP_PARAM:
        /* Read-only registers: ignore writes. */
        return;
    case CMP_CSR:
        /* CFR/CFF/RRF are write-1-to-clear; COUT is read-only. */
        s->regs[CMP_CSR / 4] &= ~(value & CMP_CSR_W1C_MASK);
        mcxn_cmp_update_irq(s);
        return;
    case CMP_IER:
        s->regs[CMP_IER / 4] = value;
        mcxn_cmp_update_irq(s);
        return;
    case CMP_RRCR0: {
        /*
         * Enabling round-robin captures the current channel-0 output as the
         * baseline (the RR_INITMOD "expected" state) -- only on the 0->1
         * transition of RR_EN, so a later reconfigure does not re-baseline.
         */
        bool was_en = s->regs[CMP_RRCR0 / 4] & CMP_RRCR0_RR_EN;
        bool now_en = value & CMP_RRCR0_RR_EN;

        s->regs[CMP_RRCR0 / 4] = value;
        if (now_en && !was_en) {
            s->rr_baseline = s->cout;
        }
        return;
    }
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

/*
 * A HARDWARE trigger routed in by INPUTMUX from CMPn_TRIG (e.g. a CTIMER
 * match): take one round-robin sample of the comparator.  Round-robin monitors
 * an input and flags when it DEVIATES from the state captured at RR_INITMOD --
 * the low-power "watch a signal, wake on change" path, paced by a timer with no
 * CPU involvement.  CSR[RRF] latches the deviation (RM: "Round-Robin Flag --
 * Detected") and, gated by IER[RRF_IE], raises the comparator IRQ.
 *
 * RRCR0[RR_TRG_SEL] picks the trigger: 0 = external (this INPUTMUX path), 1 =
 * the internal round-robin timer.  A routed external trigger while
 * internal-timer mode is selected must NOT sample.
 *
 * ⚠ Scope, stated: channel 0 only (the operator-driven `comparator-output`
 * is the single analog seam), and deviation-from-baseline.  The multi-channel
 * sweep (RRCR1[RR_CHnEN]/FIXCH/FIXP), the per-channel RRCSR[RR_CHnOUT] fan-out
 * and the internal RR timer (RRCR2) are not modelled -- an honest subset, the
 * single-channel monitor the trigger path needs.
 */
static void cmp_hw_trigger(void *opaque, int n, int level)
{
    MCXNCMPState *s = MCXN_CMP(opaque);
    uint32_t rrcr0 = s->regs[CMP_RRCR0 / 4];
    bool cur;

    if (!level) {
        /* a trigger is an edge, not a level */
        return;
    }
    if (!(rrcr0 & CMP_RRCR0_RR_EN)) {
        /* round-robin disabled */
        return;
    }
    if (rrcr0 & CMP_RRCR0_RR_TRG_SEL) {
        /* internal-timer mode: ignore the external trigger */
        return;
    }

    /*
     * Sample channel 0's (operator-driven) output; flag a deviation from
     * the baseline.
     */
    cur = s->cout;
    s->regs[CMP_RRCSR / 4] = (s->regs[CMP_RRCSR / 4] & ~CMP_RRCSR_RR_CH0OUT) |
                             (cur ? CMP_RRCSR_RR_CH0OUT : 0);
    if (cur != s->rr_baseline) {
        s->regs[CMP_CSR / 4] |= CMP_CSR_RRF;
        mcxn_cmp_update_irq(s);
    }
}

/*
 * Operator sets the comparator output level (what the +/- inputs would resolve
 * to).  A 0->1 transition latches the rising-edge flag CSR[CFR]; 1->0 latches
 * the falling-edge flag CSR[CFF]; both honour the polarity-independent edge
 * interrupt enables, raising the IRQ when armed.
 */
static void mcxn_cmp_set_cout(Object *obj, bool value, Error **errp)
{
    MCXNCMPState *s = MCXN_CMP(obj);
    uint32_t ier = s->regs[CMP_IER / 4];
    bool dma_en = (s->regs[CMP_CCR1 / 4] & CMP_CCR1_DMA_EN) != 0;
    bool edge_dma = false;

    if (value && !s->cout) {
        s->regs[CMP_CSR / 4] |= CMP_CSR_CFR;         /* rising edge  */
        edge_dma = dma_en && (ier & CMP_IER_CFR_IE);
    } else if (!value && s->cout) {
        s->regs[CMP_CSR / 4] |= CMP_CSR_CFF;         /* falling edge */
        edge_dma = dma_en && (ier & CMP_IER_CFF_IE);
    }
    s->cout = value;

    /*
     * An IER-enabled edge with DMA enabled fires the eDMA request (CMSIS
     * HsCmp{n} = 28+n) instead of the NVIC line -- a one-shot PULSE (one
     * crossing, one minor loop).
     */
    if (edge_dma) {
        qemu_irq_pulse(s->dma_req);
    }
    mcxn_cmp_update_irq(s);
}

static bool mcxn_cmp_get_cout(Object *obj, Error **errp)
{
    return MCXN_CMP(obj)->cout;
}

static const MemoryRegionOps mcxn_cmp_ops = {
    .read = mcxn_cmp_read,
    .write = mcxn_cmp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_cmp_reset(DeviceState *dev)
{
    MCXNCMPState *s = MCXN_CMP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[CMP_CCR0 / 4] = CMP_CCR0_RESET;
    s->cout = false;
    s->rr_baseline = false;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_cmp_init(Object *obj)
{
    /*
     * Expose the comparator output level as a runtime QOM property so an
     * operator can drive what the +/- inputs would resolve to:
     *   qom-set /machine/.../cmp0 comparator-output true
     */
    object_property_add_bool(obj, "comparator-output",
                             mcxn_cmp_get_cout, mcxn_cmp_set_cout);
}

static void mcxn_cmp_realize(DeviceState *dev, Error **errp)
{
    MCXNCMPState *s = MCXN_CMP(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_cmp_ops, s,
                          TYPE_MCXN_CMP, MCXN_CMP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    /* 0: NVIC comparator interrupt */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    /* 1: DMA request (edge pulse) */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req);
    /*
     * Hardware trigger routed in by INPUTMUX from CMPn_TRIG (timer ->
     * round-robin sample).
     */
    qdev_init_gpio_in_named(dev, cmp_hw_trigger, "trigger", 1);
}

/* The IRQ output line is not part of vmstate; re-drive it from the restored
 * register state so a migrated device lands with the correct level. */
static int mcxn_cmp_post_load(void *opaque, int version_id)
{
    mcxn_cmp_update_irq(MCXN_CMP(opaque));
    return 0;
}

static const VMStateDescription vmstate_mcxn_cmp = {
    .name = TYPE_MCXN_CMP,
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = mcxn_cmp_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNCMPState, MCXN_CMP_SIZE / 4),
        VMSTATE_BOOL(cout, MCXNCMPState),
        VMSTATE_BOOL(rr_baseline, MCXNCMPState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_cmp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_cmp_realize;
    device_class_set_legacy_reset(dc, mcxn_cmp_reset);
    dc->vmsd = &vmstate_mcxn_cmp;
}

static const TypeInfo mcxn_cmp_types[] = {
    {
        .name          = TYPE_MCXN_CMP,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCMPState),
        .instance_init = mcxn_cmp_init,
        .class_init    = mcxn_cmp_class_init,
    },
};

DEFINE_TYPES(mcxn_cmp_types)
