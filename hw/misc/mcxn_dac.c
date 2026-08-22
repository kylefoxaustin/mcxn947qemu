/*
 * NXP MCX N DAC (LPDAC 12-bit / HPDAC 14-bit) — functional output-FIFO model.
 * See header for the data path and the silent-wrong-answers it exists to kill.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_dac.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/visitor.h"
#include "qom/object.h"

/* --- Register offsets ----------------------------------------------------- */
#define DAC_VERID   0x00  /* RO */
#define DAC_PARAM   0x04  /* RO */
#define DAC_DATA    0x08  /* WO */
#define DAC_GCR     0x0C
#define DAC_FCR     0x10
#define DAC_FPR     0x14  /* RO */
#define DAC_FSR     0x18  /* status; OF/UF/SWBK/PTGCOCO are W1C */
#define DAC_IER     0x1C
#define DAC_DER     0x20
#define DAC_RCR     0x24
#define DAC_TCR     0x28  /* WO */
#define DAC_PCR     0x2C

/* --- GCR ------------------------------------------------------------------ */
#define GCR_DACEN   (1u << 0)
#define GCR_FIFOEN  (1u << 3)
#define GCR_SWMD    (1u << 4)
#define GCR_TRGSEL  (1u << 5)   /* 0 = hardware trigger, 1 = software trigger */
#define GCR_PTGEN   (1u << 6)

/* --- FSR (CMSIS LPDAC_FSR_*) ---------------------------------------------- */
#define FSR_FULL    (1u << 0)
#define FSR_EMPTY   (1u << 1)
#define FSR_WM      (1u << 2)   /* occupancy <= FCR[WML] */
#define FSR_SWBK    (1u << 3)
#define FSR_OF      (1u << 6)   /* overflow,  W1C */
#define FSR_UF      (1u << 7)   /* underflow, W1C */
#define FSR_PTGCOCO (1u << 8)   /* periodic-trigger queue complete, W1C */

/* FULL/EMPTY/WM are computed from occupancy; the rest latch until cleared. */
#define FSR_W1C_MASK (FSR_SWBK | FSR_OF | FSR_UF | FSR_PTGCOCO)

/* --- RCR / TCR ------------------------------------------------------------ */
#define RCR_SWRST   (1u << 0)
#define RCR_FIFORST (1u << 1)
#define TCR_SWTRG   (1u << 0)

#define DAC_VERID_VALUE 0x01000000u

static uint32_t dac_depth(MCXNDACState *s)
{
    return s->hpdac ? 32 : 16;
}

static uint32_t dac_data_mask(MCXNDACState *s)
{
    return s->hpdac ? 0x3FFFu : 0x0FFFu;   /* 14-bit HPDAC / 12-bit LPDAC */
}

/* PARAM[FIFOSZ]: depth = 2^(FIFOSZ+1) (RM §42.7.1.2). */
static uint32_t dac_param(MCXNDACState *s)
{
    return s->hpdac ? 4u : 3u;
}

/* FSR is mostly computed: only the latched flags live in regs[]. */
static uint32_t dac_fsr(MCXNDACState *s)
{
    uint32_t fsr = s->regs[DAC_FSR / 4] & FSR_W1C_MASK;
    uint32_t wml = s->regs[DAC_FCR / 4] &
                   (s->hpdac ? 0x1Fu : 0x0Fu);

    if (s->count == 0) {
        fsr |= FSR_EMPTY;
    }
    if (s->count >= dac_depth(s)) {
        fsr |= FSR_FULL;
    }
    if (s->count <= wml) {
        fsr |= FSR_WM;
    }
    return fsr;
}

/* LPDAC_DER (CMSIS): the DMA-enable bits, which line up with the FSR flags. */
#define DER_EMPTY_DMAEN  (1u << 1)   /* LPDAC_DER_EMPTY_DMAEN_MASK */
#define DER_WM_DMAEN     (1u << 2)   /* LPDAC_DER_WM_DMAEN_MASK    */

static void dac_update_irq(MCXNDACState *s)
{
    uint32_t fsr = dac_fsr(s);
    uint32_t der = s->regs[DAC_DER / 4];
    bool req;

    /* IER's bits line up one-to-one with the FSR flags. */
    qemu_set_irq(s->irq, (fsr & s->regs[DAC_IER / 4]) != 0);

    /*
     * THE DMA REQUEST LINE (mux source 25/26/27).  The FIFO asks the eDMA for
     * more samples when it has drained to the watermark (or gone empty) and the
     * matching DER bit is set — this is how a stock DAC driver streams a
     * waveform.  Without it, ERQ was a dead bit and DMA-driven DAC output could
     * not run at all.  Level-driven and edge-suppressed: a qemu_irq handler
     * runs on every qemu_set_irq call, and the eDMA re-enters us as it fills.
     */
    req = ((fsr & FSR_WM)    && (der & DER_WM_DMAEN)) ||
          ((fsr & FSR_EMPTY) && (der & DER_EMPTY_DMAEN));
    if (req != s->dma_req_level) {
        s->dma_req_level = req;
        qemu_set_irq(s->dma_req, req);
    }
}

static void dac_fifo_reset(MCXNDACState *s)
{
    s->wptr = s->rptr = s->count = 0;
    s->regs[DAC_FSR / 4] &= ~FSR_W1C_MASK;
}

/* A DATA write: push a sample (or, with the FIFO off, drive the output). */
static void dac_push(MCXNDACState *s, uint32_t value)
{
    uint32_t v = value & dac_data_mask(s);

    if (!(s->regs[DAC_GCR / 4] & GCR_FIFOEN)) {
        s->out = v;          /* buffer mode: DATA goes straight to the output */
        return;
    }

    if (s->count >= dac_depth(s)) {
        /*
         * Full: the sample is dropped and the write pointer does NOT advance
         * (RM §42.3.4).  Saying "accepted" here is how a 64-sample burst into
         * a 16-deep FIFO looks perfect in emulation and clips on the bench.
         */
        s->regs[DAC_FSR / 4] |= FSR_OF;
        dac_update_irq(s);
        return;
    }

    s->fifo[s->wptr] = v;
    s->wptr = (s->wptr + 1) % dac_depth(s);
    s->count++;
    dac_update_irq(s);
}

/* A trigger: pop one sample to the output. */
static void dac_trigger(MCXNDACState *s)
{
    if (!(s->regs[DAC_GCR / 4] & GCR_DACEN)) {
        return;
    }
    if (!(s->regs[DAC_GCR / 4] & GCR_FIFOEN)) {
        return;              /* buffer mode converts on the DATA write */
    }

    if (s->count == 0) {
        /* Underflow: the analog output holds its last value (RM §42.3.4). */
        s->regs[DAC_FSR / 4] |= FSR_UF;
        dac_update_irq(s);
        return;
    }

    s->out = s->fifo[s->rptr];
    s->rptr = (s->rptr + 1) % dac_depth(s);
    s->count--;
    dac_update_irq(s);
}

/*
 * A HARDWARE trigger routed in by INPUTMUX from DACn_TRIG (e.g. a CTIMER match
 * or an LPTMR compare): pop the next FIFO sample to the output, exactly like
 * TCR[SWTRG] does, so a timer can pace a waveform out of the DAC with zero CPU
 * involvement (the standard fsl_dac use with kDAC_ExternalTriggerMode).
 *
 * GCR[TRGSEL] picks which trigger is live: 0 = hardware (this path), 1 =
 * software (TCR[SWTRG]).  A routed hardware trigger while the DAC is in
 * SOFTWARE-trigger mode must NOT advance -- otherwise the model is more
 * permissive than silicon.  (The converse -- SWTRG in hardware mode -- is left
 * ungated, a pre-existing simplification.)
 */
static void dac_hw_trigger(void *opaque, int n, int level)
{
    MCXNDACState *s = MCXN_DAC(opaque);

    if (!level) {
        return;                  /* a trigger is an edge, not a level */
    }
    if (s->regs[DAC_GCR / 4] & GCR_TRGSEL) {
        return;      /* software-trigger mode: ignore the routed HW trigger */
    }
    dac_trigger(s);
}

static uint64_t mcxn_dac_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNDACState *s = MCXN_DAC(opaque);

    switch (offset) {
    case DAC_VERID:
        return DAC_VERID_VALUE;
    case DAC_PARAM:
        return dac_param(s);
    case DAC_DATA:
    case DAC_TCR:
        return 0;            /* write-only */
    case DAC_FPR:
        return (s->wptr << 16) | s->rptr;
    case DAC_FSR:
        return dac_fsr(s);
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_dac_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNDACState *s = MCXN_DAC(opaque);
    uint32_t v = value;

    switch (offset) {
    case DAC_VERID:
    case DAC_PARAM:
    case DAC_FPR:
        return;              /* read-only */

    case DAC_DATA:
        dac_push(s, v);
        return;

    case DAC_TCR:
        if (v & TCR_SWTRG) {
            dac_trigger(s);
        }
        return;

    case DAC_FSR:
        s->regs[DAC_FSR / 4] &= ~(v & FSR_W1C_MASK);
        dac_update_irq(s);
        return;

    case DAC_IER:
        s->regs[DAC_IER / 4] = v;
        dac_update_irq(s);
        return;

    case DAC_DER:
    case DAC_FCR:
        /*
         * Arming a DMA enable, or moving the watermark, changes whether the
         * FIFO is asking the eDMA for samples — so the request line has to be
         * re-evaluated here.  These used to fall through to a plain store, and
         * a driver that armed DER last (as a stock driver does) would never
         * raise a request at all.
         */
        s->regs[offset / 4] = v;
        dac_update_irq(s);
        return;

    case DAC_RCR:
        if (v & RCR_SWRST) {
            /* Full reset: registers, FIFO and the held output. */
            memset(s->regs, 0, sizeof(s->regs));
            dac_fifo_reset(s);
            s->out = 0;
        } else if (v & RCR_FIFORST) {
            /* FIFO only: pointers and the FIFO status flags. */
            dac_fifo_reset(s);
        }
        s->regs[DAC_RCR / 4] = 0;   /* both bits self-clear */
        dac_update_irq(s);
        return;

    case DAC_GCR:
        s->regs[DAC_GCR / 4] = v;
        if ((v & GCR_PTGEN) && !s->warned_ptg) {
            s->warned_ptg = true;
            qemu_log_mask(LOG_UNIMP,
                "mcxn-dac: GCR[PTGEN] periodic-trigger mode is not modelled; "
                "no internal triggers will be generated and FSR[PTGCOCO] will "
                "not set.  Drive the FIFO with TCR[SWTRG] instead.\n");
        }
        if ((v & GCR_SWMD) && !s->warned_swmd) {
            s->warned_swmd = true;
            qemu_log_mask(LOG_UNIMP,
                "mcxn-dac: GCR[SWMD] swing-back mode is not modelled; the read "
                "pointer will not oscillate and FSR[SWBK] will not set.\n");
        }
        dac_update_irq(s);
        return;

    default: {
        /*
         * Merge sub-word writes into the 32-bit register instead of
         * overwriting it — a byte/halfword access must not clobber the other
         * bytes of a config register (see the access-size note on the ops).
         */
        uint32_t idx = offset / 4;
        uint32_t shift = (offset & 3) * 8;
        uint32_t mask = (size >= 4) ? 0xFFFFFFFFu
                                    : (((1u << (size * 8)) - 1) << shift);
        s->regs[idx] = (s->regs[idx] & ~mask) |
                       ((uint32_t)(value << shift) & mask);
        dac_update_irq(s);
        return;
    }
    }
}

static const MemoryRegionOps mcxn_dac_ops = {
    .read = mcxn_dac_read,
    .write = mcxn_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * Accept 1/2/4-byte access: a DAC output driven over eDMA bursts 12-bit
     * samples to DATA as halfwords, and a 4-byte-only window would silently
     * drop them (fleet eDMA byte-access lesson).  impl.min=1 routes each access
     * straight to the handler (DATA masks `value`; the default merges sub-word,
     * so no config register is corrupted).
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_dac_reset(DeviceState *dev)
{
    MCXNDACState *s = MCXN_DAC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->fifo, 0, sizeof(s->fifo));
    dac_fifo_reset(s);
    s->out = 0;
    s->warned_ptg = false;
    s->warned_swmd = false;
    qemu_set_irq(s->irq, 0);
}

/*
 * The analog output pin, exposed to the OPERATOR (QMP qom-get), because that is
 * the only place a DAC's answer is observable — the guest cannot read back
 * what it converted.  This is the seam a bench engineer probes with a scope,
 * and it is how a DAC data path is verified without inventing a peer.
 * Read-only: the operator observes, the guest drives.
 */
static void dac_get_output(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    MCXNDACState *s = MCXN_DAC(obj);
    uint32_t out = s->out;

    visit_type_uint32(v, name, &out, errp);
}

static void mcxn_dac_init(Object *obj)
{
    object_property_add(obj, "analog-output", "uint32",
                        dac_get_output, NULL, NULL, NULL);
}

static void mcxn_dac_realize(DeviceState *dev, Error **errp)
{
    MCXNDACState *s = MCXN_DAC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_dac_ops, s,
                          TYPE_MCXN_DAC, MCXN_DAC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req);   /* -> eDMA src 25+i */
    /*
     * Hardware trigger routed in by INPUTMUX from DACn_TRIG (timer/PWM ->
     * waveform).
     */
    qdev_init_gpio_in_named(dev, dac_hw_trigger, "trigger", 1);
}

static const Property mcxn_dac_properties[] = {
    /* DAC2 is the high-performance part: 14-bit samples, 32-deep FIFO. */
    DEFINE_PROP_BOOL("hpdac", MCXNDACState, hpdac, false),
};

/* Re-drive the IRQ line from restored register state after migration (the
 * output line is not migrated); without this a VM migrated with FSR & IER
 * asserted lands with the line low and the guest's level IRQ lost. */
static int mcxn_dac_post_load(void *opaque, int version_id)
{
    dac_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_mcxn_dac = {
    .name = TYPE_MCXN_DAC,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = mcxn_dac_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNDACState, MCXN_DAC_SIZE / 4),
        VMSTATE_UINT16_ARRAY(fifo, MCXNDACState, MCXN_DAC_FIFO_MAX),
        VMSTATE_UINT32(wptr, MCXNDACState),
        VMSTATE_UINT32(rptr, MCXNDACState),
        VMSTATE_UINT32(count, MCXNDACState),
        VMSTATE_UINT32(out, MCXNDACState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_dac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_dac_realize;
    device_class_set_legacy_reset(dc, mcxn_dac_reset);
    dc->vmsd = &vmstate_mcxn_dac;
    device_class_set_props(dc, mcxn_dac_properties);
}

static const TypeInfo mcxn_dac_types[] = {
    {
        .name          = TYPE_MCXN_DAC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNDACState),
        .instance_init = mcxn_dac_init,
        .class_init    = mcxn_dac_class_init,
    },
};

DEFINE_TYPES(mcxn_dac_types)
