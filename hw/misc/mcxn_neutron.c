/*
 * NXP MCX N eIQ Neutron NPU (neural accelerator) — behavioural model.
 *
 * See include/hw/misc/mcxn_neutron.h for the design + sources.  Summary: the
 * Neutron compute path is proprietary microcode (RM §20.4: no user-configurable
 * registers; SDK ships binary blobs), so this model honours the host-visible
 * CTRL/INTR handshake just enough that the eIQ driver's exec/done spins complete
 * — but it does NOT compute the inference.  That is exposed honestly
 * (FLAG-AT-OPERATOR): compute-modelled=false + a jobs-started counter over QMP,
 * a LOG_UNIMP per kick, and an optional operator-driven guest-visible error
 * trap.  Never a silent wrong answer; never a hang.
 *
 * ⚠ WHAT REAL NEUTRON SILICON DOES, AND WHY THIS MODEL IS DELIBERATELY *MORE
 * HONEST THAN THE HARDWARE* (measured on shipped silicon by the 95 fleet, 2026-07-12):
 *
 *   Real Neutron DOES NOT REFUSE WORK IT CANNOT DO CORRECTLY.  It CLAIMS the op
 *   and returns garbage.  Measured: an 8-bit MatMulNBits at a KNOWN-SAFE K was
 *   accepted by the NPU execution provider and came back with rel-L2 = 103% and
 *   cosine = -0.0019 — i.e. ORTHOGONAL TO THE TRUTH, not merely inaccurate.  And
 *   at certain K values (tiling-dependent, NON-MONOTONIC: 8192 is fine, 7168 is
 *   garbage; 10752 is fine, 11008 is garbage) it produces a numerical explosion.
 *   Even when it IS correct, its error is ~6x the ggml CPU kernel it replaces and
 *   costs +7.3% perplexity end-to-end.
 *
 *   This model instead FAULTS to the guest (INTR[ERRORTRAP]), because the compute
 *   is proprietary microcode with no user-visible registers: we cannot reproduce
 *   the right answer, and we will not fabricate a plausible wrong one.
 *
 *   ⇒ THEREFORE: A CLEAN ERRORTRAP HERE IS *NOT* A PROMISE THAT SILICON WILL
 *     FAULT.  Firmware that "handled the error nicely" against this model will,
 *     on a real board, be handed CONFIDENT NONSENSE instead — with no flag, no
 *     interrupt, and nothing to catch.  Validate accelerator numerics on silicon;
 *     the emulator can tell you the op was ATTEMPTED, never that it was RIGHT.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_neutron.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* Register offsets (host-visible handshake; from the M33 driver disassembly). */
#define R_CTRL      0x00    /* write kicks a step; bit31 = SHADOW_BUSY        */
#define R_INTR      0x40    /* INTREN/EVENTEN/ERRORTRAP control               */

/* CTRL bits. */
#define CTRL_SHADOW_BUSY  (1u << 31)

/* INTR bits (NEUTRON_Type, i.MX95 header — same IP family). */
#define INTR_INTREN       (1u << 0)
#define INTR_EVENTEN      (1u << 1)
#define INTR_ERRORTRAP_M  (1u << 2)
#define INTR_ERRORTRAP_R  (1u << 3)

static void mcxn_neutron_update_irq(MCXNNeutronState *s)
{
    /* Only the operator-opt-in error trap ever drives the line; the eIQ
     * completion path uses the CPU EVENT/WFE, not this NVIC IRQ. */
    bool active = (s->regs[R_INTR / 4] & (INTR_ERRORTRAP_M | INTR_ERRORTRAP_R)) &&
                  (s->regs[R_INTR / 4] & INTR_INTREN);
    qemu_set_irq(s->irq, active);
}

static uint64_t mcxn_neutron_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNNeutronState *s = MCXN_NEUTRON(opaque);
    uint32_t v = (off < MCXN_NEUTRON_SIZE) ? s->regs[off >> 2] : 0;

    if (off == R_CTRL) {
        /* The step always retires instantly: never read back busy, read idle.
         * This makes neutron_exec() (while bit31) and neutron_done() (while
         * != 0) both fall through without the WFE ever blocking. */
        return 0;
    }
    return v;
}

static void mcxn_neutron_write(void *opaque, hwaddr off, uint64_t value,
                               unsigned size)
{
    MCXNNeutronState *s = MCXN_NEUTRON(opaque);
    uint32_t val = value;

    if (off >= MCXN_NEUTRON_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_CTRL:
        /*
         * A microcode step was kicked.  We can't run the proprietary Neutron
         * compute, so ack it (idle CTRL so exec/done complete) but record that
         * the result is UNCOMPUTED — never silently fabricate output.
         */
        s->regs[R_CTRL / 4] = 0;                 /* retire instantly (idle) */
        s->jobs_started++;
        qemu_log_mask(LOG_UNIMP,
                      "%s: Neutron compute step acked but NOT modelled — "
                      "inference result UNCOMPUTED (compute-modelled=false, "
                      "jobs-started=%u)\n", __func__, s->jobs_started);
        if (s->uncomputed_errortrap) {
            /* Operator opted in: surface the uncomputed result to the guest via
             * the non-gating error-trap channel (never the completion gate). */
            s->regs[R_INTR / 4] |= INTR_ERRORTRAP_M;
            mcxn_neutron_update_irq(s);
        }
        return;
    case R_INTR:
        s->regs[R_INTR / 4] = val;
        mcxn_neutron_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = val;
        return;
    }
}

static const MemoryRegionOps mcxn_neutron_ops = {
    .read = mcxn_neutron_read,
    .write = mcxn_neutron_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_neutron_reset(DeviceState *dev)
{
    MCXNNeutronState *s = MCXN_NEUTRON(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->jobs_started = 0;
    mcxn_neutron_update_irq(s);
}

/* Honesty marker: the Neutron microcode/compute is never executed. */
static bool mcxn_neutron_compute_modelled(Object *obj, Error **errp)
{
    return false;
}

static void mcxn_neutron_init(Object *obj)
{
    MCXNNeutronState *s = MCXN_NEUTRON(obj);

    /* Farm-control-plane visibility (qom-get): the engine never computes, and
     * how many inference steps were acked-but-not-computed. */
    object_property_add_bool(obj, "compute-modelled",
                             mcxn_neutron_compute_modelled, NULL);
    object_property_add_uint32_ptr(obj, "jobs-started",
                                   &s->jobs_started, OBJ_PROP_FLAG_READ);
}

static void mcxn_neutron_realize(DeviceState *dev, Error **errp)
{
    MCXNNeutronState *s = MCXN_NEUTRON(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_neutron_ops, s,
                          TYPE_MCXN_NEUTRON, MCXN_NEUTRON_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_neutron = {
    .name = TYPE_MCXN_NEUTRON,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNNeutronState, MCXN_NEUTRON_SIZE / 4),
        VMSTATE_UINT32(jobs_started, MCXNNeutronState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mcxn_neutron_props[] = {
    /*
     * Surface "uncomputed" to the GUEST via INTR.ERRORTRAP + NPU IRQ 97, on by
     * default.
     *
     * This used to default off ("faithful-ack": firmware runs to completion and
     * the uncomputed result is flagged only via QMP/log).  That is a silent
     * wrong answer, and it is the exact class this project exists to kill: the
     * guest kicks an inference, we ack DONE, we never write the output buffer,
     * and firmware reads whatever was there — plausible-looking zeros — with no
     * way to tell.  The truth existed only on the HOST side, where the firmware
     * under test cannot see it.  An accelerator that acks DONE without computing
     * is a lie told to the guest (fleet position, ratified by 95emulator, who
     * flipped the same default for the same reason).
     *
     * Set uncomputed-errortrap=false only if you knowingly want the old
     * ack-and-say-nothing behaviour.
     *
     * The channel matters as much as the flag: this is the NON-GATING error trap,
     * never the completion gate.  Faulting an NXP accelerator through its
     * completion retcode hangs the driver instead of informing it (fleet finding;
     * see the Neutron honest-fault recipe).
     */
    DEFINE_PROP_BOOL("uncomputed-errortrap", MCXNNeutronState,
                     uncomputed_errortrap, true),
};

static void mcxn_neutron_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_neutron_realize;
    device_class_set_legacy_reset(dc, mcxn_neutron_reset);
    device_class_set_props(dc, mcxn_neutron_props);
    dc->vmsd = &vmstate_mcxn_neutron;
}

static const TypeInfo mcxn_neutron_types[] = {
    {
        .name          = TYPE_MCXN_NEUTRON,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNNeutronState),
        .instance_init = mcxn_neutron_init,
        .class_init    = mcxn_neutron_class_init,
    },
};

DEFINE_TYPES(mcxn_neutron_types)
