/*
 * NXP MCX N SINC (sigma-delta / sinc filter) — functional model.  See header
 * for the data path and why the register-fed (PM/SM) bitstream lets this block
 * compute real results.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_sinc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* --- Top-level register offsets (CMSIS SINC_Type) -------------------------- */
#define SINC_VERID      0x00  /* RO */
#define SINC_PARAMETER  0x04  /* RO */
#define SINC_MCR        0x08
#define SINC_NIE        0x0C
#define SINC_EIE        0x10
#define SINC_FIFOIE     0x14
#define SINC_NIS        0x18  /* W1C */
#define SINC_EIS        0x1C  /* W1C */
#define SINC_FIFOIS     0x20  /* W1C */
#define SINC_SR         0x24  /* RO, computed */

/* --- Channel array: 5 channels, base 0x38, step 0x30 ----------------------- */
#define SINC_CH_BASE    0x38
#define SINC_CH_STEP    0x30
#define SINC_CH_LAST    (SINC_CH_BASE + MCXN_SINC_NUM_CH * SINC_CH_STEP)

/* Channel-relative offsets. */
#define SINC_CH_CCR     0x00
#define SINC_CH_CDR     0x04
#define SINC_CH_CCFR    0x08
#define SINC_CH_CBIAS   0x10
#define SINC_CH_CRDATA  0x1C  /* RO  */
#define SINC_CH_CMPDATA 0x20  /* the register-fed bitstream */
#define SINC_CH_CSR     0x28
#define SINC_CH_CDBGR   0x2C  /* RO  */

/* --- Identity registers: reset values from RM §47.7.1 / §47.7.3 ------------ */
/* MAJOR=2, MINOR=0, FEATURE=0x7D */
#define SINC_VERID_VALUE      0x0200007Du
/* PF_ORD_SEL=10b (max order 3), FLT_NUM=5, FIFO_DEPTH=8 */
#define SINC_PARAMETER_VALUE  0x00700508u

/* --- MCR ------------------------------------------------------------------- */
#define MCR_STRIG(n)    (1u << (n))     /* STRIG0..4: software trigger */
#define MCR_STRIG_ALL   0x1Fu
#define MCR_RST         (1u << 13)
#define MCR_MEN         (1u << 15)
#define MCR_MCLK0DIS    (1u << 27)
#define MCR_MCLK1DIS    (1u << 28)
#define MCR_MCLK2DIS    (1u << 29)

/* --- SR (all computed) ----------------------------------------------------- */
#define SR_CIP(n)       (1u << (n))          /* conversion in progress  */
#define SR_CHRDY(n)     (1u << (8 + (n)))    /* channel ready           */
#define SR_FIFOEMPTY(n) (1u << (16 + (n)))   /* channel FIFO empty      */
#define SR_MCLKRDY(n)   (1u << (24 + (n)))   /* modulator clock ready   */

/* --- NIS / NIE ------------------------------------------------------------- */
#define NIS_COC(n)      (1u << (n))          /* conversion complete     */
#define NIS_CHF(n)      (1u << (8 + (n)))    /* FIFO watermark passed   */

/* --- CCR ------------------------------------------------------------------- */
#define CCR_CHEN        (1u << 0)
#define CCR_PFEN        (1u << 1)
#define CCR_FIFOEN      (1u << 14)

/* --- CDR ------------------------------------------------------------------- */
#define CDR_PFOSR(v)    ((v) & 0x7FF)
#define CDR_PFORD(v)    (((v) >> 11) & 0x3)
#define CDR_PFCM(v)     (((v) >> 14) & 0x3)
#define PFCM_SINGLE     0
#define PFCM_CONTINUOUS 1
#define PFCM_ALWAYS     2
#define PFCM_FIXEDNUM   3

/* --- CCFR ------------------------------------------------------------------ */
#define CCFR_PFSFT(v)   ((v) & 0x1F)
#define CCFR_RDFMT      (1u << 6)             /* 0 = signed, 1 = unsigned */
#define CCFR_FIFOWMK(v) (((v) >> 10) & 0x7)
#define CCFR_IBFMT(v)   (((v) >> 16) & 0x3)
#define IBFMT_EM        0    /* external modulator pins            */
#define IBFMT_EXT       1    /* external bitstream                 */
#define IBFMT_PM        2    /* parallel mode: CnMPDATA low 16 bits */
#define IBFMT_SM        3    /* serial mode:  CnMPDATA all 32 bits  */

/* --- FIFOIS / FIFOIE (CMSIS: FUNF at 0..4, FOVF at 8..12) ------------------ */
#define FIFO_UNDERFLOW(n) (1u << (n))
#define FIFO_OVERFLOW(n)  (1u << (8 + (n)))

/* --- CSR ------------------------------------------------------------------- */
#define CSR_FIFOAVIL    0x1Fu
#define CSR_PSRDY       (1u << 7)
#define CSR_PFSAT       (1u << 8)
#define CSR_SRDS        (1u << 13)
#define CSR_DBGRS       (3u << 14)

static uint32_t sinc_ch_reg(MCXNSINCState *s, int n, unsigned rel)
{
    return s->regs[(SINC_CH_BASE + n * SINC_CH_STEP + rel) / 4];
}

static void sinc_set_ch_reg(MCXNSINCState *s, int n, unsigned rel, uint32_t v)
{
    s->regs[(SINC_CH_BASE + n * SINC_CH_STEP + rel) / 4] = v;
}

/* The modulator clocks come up shortly after MCR[MEN]; without them the stock
 * SDK's SINC_Init() spins forever waiting on SR[MCLKRDYn]. */
static bool sinc_mclk_ready(MCXNSINCState *s, int clk)
{
    static const uint32_t dis[3] = { MCR_MCLK0DIS, MCR_MCLK1DIS, MCR_MCLK2DIS };
    uint32_t mcr = s->regs[SINC_MCR / 4];

    return (mcr & MCR_MEN) && !(mcr & dis[clk]);
}

static void sinc_update_irq(MCXNSINCState *s)
{
    bool level = (s->regs[SINC_NIS / 4]    & s->regs[SINC_NIE / 4])    ||
                 (s->regs[SINC_EIS / 4]    & s->regs[SINC_EIE / 4])    ||
                 (s->regs[SINC_FIFOIS / 4] & s->regs[SINC_FIFOIE / 4]);

    qemu_set_irq(s->irq, level);
}

static void sinc_reset_channel_filter(MCXNSINCChannel *c)
{
    memset(c->integ, 0, sizeof(c->integ));
    memset(c->comb, 0, sizeof(c->comb));
    memset(c->fs_delay, 0, sizeof(c->fs_delay));
    c->phase = 0;
    c->fifo_count = 0;
    c->last = 0;
    c->have_last = false;
}

/* Push a completed 24-bit result and raise the completion / watermark flags. */
static void sinc_push_result(MCXNSINCState *s, int n, int64_t v)
{
    MCXNSINCChannel *c = &s->ch[n];
    uint32_t ccr  = sinc_ch_reg(s, n, SINC_CH_CCR);
    uint32_t ccfr = sinc_ch_reg(s, n, SINC_CH_CCFR);
    uint32_t csr  = sinc_ch_reg(s, n, SINC_CH_CSR);
    uint32_t wmk  = CCFR_FIFOWMK(ccfr);
    int32_t  res;

    /* Saturate into the 24-bit result field, and say so (CSR[PFSAT]) rather
     * than silently wrapping. */
    if (ccfr & CCFR_RDFMT) {                 /* unsigned */
        if (v < 0)          { v = 0;          csr |= CSR_PFSAT; }
        if (v > 0xFFFFFF)   { v = 0xFFFFFF;   csr |= CSR_PFSAT; }
    } else {                                 /* signed */
        if (v < -0x800000)  { v = -0x800000;  csr |= CSR_PFSAT; }
        if (v > 0x7FFFFF)   { v = 0x7FFFFF;   csr |= CSR_PFSAT; }
    }
    res = (int32_t)v;

    if (ccr & CCR_FIFOEN) {
        if (c->fifo_count < MCXN_SINC_FIFO_DEPTH) {
            c->fifo[c->fifo_count++] = res;
        } else {
            /* FIFO full: the sample is lost.  Flag it (FIFOIS[FOVFn]) —
             * dropping it silently would hide a real-time budget that does not
             * close. */
            s->regs[SINC_FIFOIS / 4] |= FIFO_OVERFLOW(n);
        }
    } else {
        c->last = res;
        c->have_last = true;
    }
    sinc_set_ch_reg(s, n, SINC_CH_CSR, csr);

    s->regs[SINC_NIS / 4] |= NIS_COC(n);
    if ((ccr & CCR_FIFOEN) && c->fifo_count > wmk) {
        s->regs[SINC_NIS / 4] |= NIS_CHF(n);

        /* Fixed-Number mode runs until the watermark is reached (§47.3.2, the
         * Modes and behaviors table). */
        if (CDR_PFCM(sinc_ch_reg(s, n, SINC_CH_CDR)) == PFCM_FIXEDNUM) {
            c->running = false;
        }
    }

    if (CDR_PFCM(sinc_ch_reg(s, n, SINC_CH_CDR)) == PFCM_SINGLE) {
        c->running = false;      /* one trigger, one conversion */
    }

    sinc_update_irq(s);
}

/*
 * Feed one modulator bit into a channel's CIC.  This is the RM's transfer
 * function, implemented literally: ORD integrators at the bitstream rate, a
 * decimate-by-OSR, then ORD combs at the output rate.
 */
static void sinc_feed_bit(MCXNSINCState *s, int n, int bit)
{
    MCXNSINCChannel *c = &s->ch[n];
    uint32_t cdr  = sinc_ch_reg(s, n, SINC_CH_CDR);
    uint32_t ccfr = sinc_ch_reg(s, n, SINC_CH_CCFR);
    uint32_t osr  = CDR_PFOSR(cdr) + 1;
    uint32_t pford = CDR_PFORD(cdr);
    bool fastsinc = (pford == 0);
    uint32_t ord  = fastsinc ? 2 : pford;
    int64_t  x, v;
    int32_t  shift = CCFR_PFSFT(ccfr);
    int32_t  bias  = (int32_t)sinc_ch_reg(s, n, SINC_CH_CBIAS);

    /* An unsigned stream is {0,1}; a signed one is {-1,+1} (RM Eq. 22, where
     * the signed format needs exactly one extra bit of output width). */
    x = (ccfr & CCFR_RDFMT) ? bit : (bit ? 1 : -1);

    /* Integrator chain, at the input rate. */
    v = x;
    for (uint32_t k = 0; k < ord; k++) {
        c->integ[k] += v;
        v = c->integ[k];
    }

    if (++c->phase < osr) {
        return;                  /* still accumulating this decimation window */
    }
    c->phase = 0;

    /* Comb (differentiator) chain, at the decimated rate. */
    v = c->integ[ord - 1];
    for (uint32_t k = 0; k < ord; k++) {
        int64_t d = v - c->comb[k];

        c->comb[k] = v;
        v = d;
    }

    if (fastsinc) {
        /* (1 + z^-2*OSR) at the input rate is (1 + z^-2) at the output rate. */
        int64_t y = v + c->fs_delay[1];

        c->fs_delay[1] = c->fs_delay[0];
        c->fs_delay[0] = v;
        v = y;
    }

    v >>= shift;                 /* CnCFR[PFSFT] scales the CIC's gain down */
    v += bias;                   /* CnBIAS                                   */

    sinc_push_result(s, n, v);
}

/* A write to CnMPDATA in PM/SM mode is the bitstream, MSB first. */
static void sinc_feed_mpdata(MCXNSINCState *s, int n, uint32_t value)
{
    MCXNSINCChannel *c = &s->ch[n];
    uint32_t ccr  = sinc_ch_reg(s, n, SINC_CH_CCR);
    uint32_t ibfmt = CCFR_IBFMT(sinc_ch_reg(s, n, SINC_CH_CCFR));
    int nbits;

    if (!(s->regs[SINC_MCR / 4] & MCR_MEN) ||
        !(ccr & CCR_CHEN) || !(ccr & CCR_PFEN)) {
        return;                  /* filter not enabled: no clocks, no samples */
    }
    if (!c->running) {
        return;                  /* every PFCM mode needs a trigger first */
    }

    switch (ibfmt) {
    case IBFMT_PM: nbits = 16; break;   /* low 16 bits  (§47.3.2.3.7) */
    case IBFMT_SM: nbits = 32; break;   /* all 32 bits  (§47.3.2.3.8) */
    default:
        /* IBFMT selects an external modulator pin; a write to CnMPDATA is not
         * the bitstream source in that case. */
        return;
    }

    for (int i = nbits - 1; i >= 0; i--) {
        sinc_feed_bit(s, n, (value >> i) & 1);
    }
}

static uint64_t mcxn_sinc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSINCState *s = MCXN_SINC(opaque);
    uint32_t v;
    int n;
    unsigned rel;

    switch (offset) {
    case SINC_VERID:
        return SINC_VERID_VALUE;
    case SINC_PARAMETER:
        return SINC_PARAMETER_VALUE;
    case SINC_SR:
        /* Every SR bit reflects real state.  In particular FIFOEMPTY is 1 when
         * the FIFO really is empty: reporting "not empty" unconditionally (as
         * the bring-up model did) hands firmware an endless run of zeros that
         * is indistinguishable from real samples. */
        v = 0;
        for (n = 0; n < MCXN_SINC_NUM_CH; n++) {
            uint32_t ccr = sinc_ch_reg(s, n, SINC_CH_CCR);

            if (s->ch[n].running) {
                v |= SR_CIP(n);
            }
            if ((ccr & CCR_CHEN) && (s->regs[SINC_MCR / 4] & MCR_MEN)) {
                v |= SR_CHRDY(n);
            }
            if (s->ch[n].fifo_count == 0) {
                v |= SR_FIFOEMPTY(n);
            }
        }
        for (n = 0; n < 3; n++) {
            if (sinc_mclk_ready(s, n)) {
                v |= SR_MCLKRDY(n);
            }
        }
        return v;
    default:
        break;
    }

    if (offset >= SINC_CH_BASE && offset < SINC_CH_LAST) {
        n   = (offset - SINC_CH_BASE) / SINC_CH_STEP;
        rel = (offset - SINC_CH_BASE) % SINC_CH_STEP;

        switch (rel) {
        case SINC_CH_CRDATA: {
            MCXNSINCChannel *c = &s->ch[n];
            uint32_t ccr = sinc_ch_reg(s, n, SINC_CH_CCR);
            int32_t  res;

            if (ccr & CCR_FIFOEN) {
                if (c->fifo_count == 0) {
                    /* Underflow: nothing has been converted.  Flag it
                     * (FIFOIS[FUNFn]) rather than return a plausible zero. */
                    s->regs[SINC_FIFOIS / 4] |= FIFO_UNDERFLOW(n);
                    sinc_update_irq(s);
                    return 0;
                }
                res = c->fifo[0];
                memmove(&c->fifo[0], &c->fifo[1],
                        (--c->fifo_count) * sizeof(c->fifo[0]));
                /* Draining below the watermark retires CHF. */
                if (c->fifo_count <=
                    CCFR_FIFOWMK(sinc_ch_reg(s, n, SINC_CH_CCFR))) {
                    s->regs[SINC_NIS / 4] &= ~NIS_CHF(n);
                    sinc_update_irq(s);
                }
            } else {
                if (!c->have_last) {
                    return 0;
                }
                res = c->last;
            }
            /* RDATA occupies bits [31:8] (CMSIS SINC_CRDATA_RDATA_MASK). */
            return ((uint32_t)res << 8) & 0xFFFFFF00u;
        }

        case SINC_CH_CSR: {
            uint32_t csr = sinc_ch_reg(s, n, SINC_CH_CSR);

            csr &= ~CSR_FIFOAVIL;
            csr |= s->ch[n].fifo_count & CSR_FIFOAVIL;
            /* We consume a PM/SM write immediately, so the filter is always
             * ready for the next one. */
            csr |= CSR_PSRDY;
            /* SRDS / DBGRS are self-clearing requests: the hardware clears them
             * when the (instant) request completes.  Leaving them set is what
             * hangs SINC_DoSoftwareReadDebugData(). */
            csr &= ~(CSR_SRDS | CSR_DBGRS);
            return csr;
        }

        case SINC_CH_CDBGR:
            return 0;

        default:
            return s->regs[offset / 4];
        }
    }

    return s->regs[offset / 4];
}

static void mcxn_sinc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNSINCState *s = MCXN_SINC(opaque);
    uint32_t v = value;
    int n;
    unsigned rel;

    switch (offset) {
    case SINC_VERID:
    case SINC_PARAMETER:
    case SINC_SR:
        return;  /* read-only */

    case SINC_MCR:
        if (v & MCR_RST) {
            for (n = 0; n < MCXN_SINC_NUM_CH; n++) {
                sinc_reset_channel_filter(&s->ch[n]);
                s->ch[n].running = false;
            }
            v &= ~MCR_RST;    /* self-clearing */
        }
        /* STRIGn starts a conversion.  Every PFCM mode needs a trigger; Always
         * mode ignores further triggers, the others restart. */
        for (n = 0; n < MCXN_SINC_NUM_CH; n++) {
            if (v & MCR_STRIG(n)) {
                MCXNSINCChannel *c = &s->ch[n];
                uint32_t cdr = sinc_ch_reg(s, n, SINC_CH_CDR);

                if (!(CDR_PFCM(cdr) == PFCM_ALWAYS && c->running)) {
                    sinc_reset_channel_filter(c);
                    c->running = true;
                }
            }
        }
        v &= ~MCR_STRIG_ALL;  /* triggers are self-clearing */
        s->regs[SINC_MCR / 4] = v;
        return;

    case SINC_NIE:
    case SINC_EIE:
    case SINC_FIFOIE:
        s->regs[offset / 4] = v;
        sinc_update_irq(s);
        return;

    case SINC_NIS:
    case SINC_EIS:
    case SINC_FIFOIS:
        s->regs[offset / 4] &= ~v;   /* W1C */
        sinc_update_irq(s);
        return;

    default:
        break;
    }

    if (offset >= SINC_CH_BASE && offset < SINC_CH_LAST) {
        n   = (offset - SINC_CH_BASE) / SINC_CH_STEP;
        rel = (offset - SINC_CH_BASE) % SINC_CH_STEP;

        switch (rel) {
        case SINC_CH_CRDATA:
        case SINC_CH_CDBGR:
            return;              /* read-only */

        case SINC_CH_CMPDATA:
            s->regs[offset / 4] = v;
            sinc_feed_mpdata(s, n, v);
            return;

        case SINC_CH_CCR: {
            uint32_t old = sinc_ch_reg(s, n, SINC_CH_CCR);

            s->regs[offset / 4] = v;
            /* Toggling CHEN resets the filter (RM §47.3.2.3.8 note). */
            if ((old & CCR_CHEN) && !(v & CCR_CHEN)) {
                sinc_reset_channel_filter(&s->ch[n]);
                s->ch[n].running = false;
            }
            return;
        }

        case SINC_CH_CCFR: {
            uint32_t ibfmt = CCFR_IBFMT(v);

            s->regs[offset / 4] = v;
            if ((ibfmt == IBFMT_EM || ibfmt == IBFMT_EXT) &&
                !s->warned_ext_source) {
                s->warned_ext_source = true;
                qemu_log_mask(LOG_UNIMP,
                    "mcxn-sinc: ch%d selected an EXTERNAL modulator bitstream "
                    "(CnCFR[IBFMT]=%u); there is no analog source in emulation, "
                    "so no samples will be produced.  Use the register-fed PM "
                    "(IBFMT=2) or SM (IBFMT=3) mode to drive the filter.\n",
                    n, ibfmt);
            }
            return;
        }

        default:
            break;
        }
    }

    /* Merge sub-word writes so a byte/halfword access can't clobber the other
     * bytes of a config register (see the access-size note on the ops). */
    {
        uint32_t idx = offset / 4;
        uint32_t shift = (offset & 3) * 8;
        uint32_t mask = (size >= 4) ? 0xFFFFFFFFu
                                    : (((1u << (size * 8)) - 1) << shift);
        s->regs[idx] = (s->regs[idx] & ~mask) |
                       ((uint32_t)(value << shift) & mask);
    }
}

static const MemoryRegionOps mcxn_sinc_ops = {
    .read = mcxn_sinc_read,
    .write = mcxn_sinc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * Accept 1/2/4-byte access: SINC result channels are drained over eDMA,
     * which can burst sub-word reads; a 4-byte-only window would reject them
     * (fleet eDMA byte-access lesson).  The write default merges sub-word so no
     * config register is corrupted.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_sinc_reset(DeviceState *dev)
{
    MCXNSINCState *s = MCXN_SINC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (int n = 0; n < MCXN_SINC_NUM_CH; n++) {
        sinc_reset_channel_filter(&s->ch[n]);
        s->ch[n].running = false;
    }
    s->warned_ext_source = false;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_sinc_realize(DeviceState *dev, Error **errp)
{
    MCXNSINCState *s = MCXN_SINC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_sinc_ops, s,
                          TYPE_MCXN_SINC, MCXN_SINC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_sinc_channel = {
    .name = "mcxn-sinc-channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64_ARRAY(integ, MCXNSINCChannel, MCXN_SINC_MAX_ORD),
        VMSTATE_INT64_ARRAY(comb, MCXNSINCChannel, MCXN_SINC_MAX_ORD),
        VMSTATE_INT64_ARRAY(fs_delay, MCXNSINCChannel, 2),
        VMSTATE_UINT32(phase, MCXNSINCChannel),
        VMSTATE_BOOL(running, MCXNSINCChannel),
        VMSTATE_INT32_ARRAY(fifo, MCXNSINCChannel, MCXN_SINC_FIFO_DEPTH),
        VMSTATE_UINT32(fifo_count, MCXNSINCChannel),
        VMSTATE_INT32(last, MCXNSINCChannel),
        VMSTATE_BOOL(have_last, MCXNSINCChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_mcxn_sinc = {
    .name = TYPE_MCXN_SINC,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSINCState, MCXN_SINC_SIZE / 4),
        VMSTATE_STRUCT_ARRAY(ch, MCXNSINCState, MCXN_SINC_NUM_CH, 1,
                             vmstate_mcxn_sinc_channel, MCXNSINCChannel),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_sinc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_sinc_realize;
    device_class_set_legacy_reset(dc, mcxn_sinc_reset);
    dc->vmsd = &vmstate_mcxn_sinc;
}

static const TypeInfo mcxn_sinc_types[] = {
    {
        .name          = TYPE_MCXN_SINC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSINCState),
        .class_init    = mcxn_sinc_class_init,
    },
};

DEFINE_TYPES(mcxn_sinc_types)
