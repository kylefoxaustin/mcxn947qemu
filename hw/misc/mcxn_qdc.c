/*
 * NXP MCX N QDC (Quadrature Decoder / ENC) — bring-up model.  See header for
 * the modelled semantics.  One QOM type ("mcxn-qdc") instantiated for QDC0 and
 * QDC1.
 *
 * Register layout from the MCXN947 CMSIS header (QDC_Type).  Offsets/bit masks
 * are taken verbatim from that header; reset values default to zero (the RM
 * gives 0x0000 reset for the registers exercised at bring-up).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_qdc.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS QDC_Type). */
#define QDC_CTRL    0x00    /* Control */
#define QDC_REV     0x0A    /* Revolution counter */
#define QDC_REVH    0x0C    /* Revolution hold (RO) */
#define QDC_UPOS    0x0E    /* Upper position counter */
#define QDC_LPOS    0x10    /* Lower position counter */
#define QDC_UPOSH   0x12    /* Upper position hold (RO) */
#define QDC_LPOSH   0x14    /* Lower position hold (RO) */
#define QDC_CTRL2   0x1E    /* Control 2 */

/* CTRL: SWIP is a software-trigger pulse that hardware self-clears. */
#define QDC_CTRL_SWIP_MASK   0x0800u
/* CTRL: write-1-to-clear interrupt flags (CMPIRQ/DIRQ/XIRQ/HIRQ). */
#define QDC_CTRL_W1C_MASK    (0x0002u | 0x0010u | 0x0100u | 0x8000u)
/* CTRL2: write-1-to-clear interrupt flags (RUIRQ/ROIRQ/SABIRQ). */
#define QDC_CTRL2_W1C_MASK   (0x0020u | 0x0080u | 0x0800u)
/* CTRL2 trigger-behaviour selects (CMSIS QDC_CTRL2_*). */
#define QDC_CTRL2_UPDHLD     0x0001u   /* on a trigger, snapshot POS/REV -> hold regs */
#define QDC_CTRL2_UPDPOS     0x0002u   /* on a trigger, clear the POS/REV counters */

static inline uint32_t qdc_ld16(MCXNQDCState *s, hwaddr off)
{
    return s->regs[off] | ((uint32_t)s->regs[off + 1] << 8);
}

static inline void qdc_st16(MCXNQDCState *s, hwaddr off, uint16_t v)
{
    s->regs[off] = v & 0xff;
    s->regs[off + 1] = (v >> 8) & 0xff;
}

static uint64_t mcxn_qdc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNQDCState *s = MCXN_QDC(opaque);
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_QDC_SIZE) {
            val |= (uint64_t)s->regs[offset + i] << (8 * i);
        }
    }
    return val;
}

static void mcxn_qdc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNQDCState *s = MCXN_QDC(opaque);
    unsigned i;

    if (size == 2 && offset == QDC_CTRL) {
        uint16_t v = value & 0xffff;
        uint16_t cur = qdc_ld16(s, QDC_CTRL);
        /* Clear the W1C interrupt flags that software wrote 1 to. */
        cur &= ~(v & QDC_CTRL_W1C_MASK);
        /* Apply the remaining (configuration) bits from the new value. */
        cur = (cur & QDC_CTRL_W1C_MASK) | (v & ~QDC_CTRL_W1C_MASK);
        /* SWIP is a self-clearing software trigger: never reads back set. */
        cur &= ~QDC_CTRL_SWIP_MASK;
        qdc_st16(s, QDC_CTRL, cur);
        return;
    }

    if (size == 2 && offset == QDC_CTRL2) {
        uint16_t v = value & 0xffff;
        uint16_t cur = qdc_ld16(s, QDC_CTRL2);
        cur &= ~(v & QDC_CTRL2_W1C_MASK);
        cur = (cur & QDC_CTRL2_W1C_MASK) | (v & ~QDC_CTRL2_W1C_MASK);
        qdc_st16(s, QDC_CTRL2, cur);
        return;
    }

    for (i = 0; i < size; i++) {
        if (offset + i < MCXN_QDC_SIZE) {
            s->regs[offset + i] = (value >> (8 * i)) & 0xff;
        }
    }
}

/*
 * A HARDWARE trigger routed in by INPUTMUX from QDCn_TRIG (e.g. a CTIMER match or a
 * FlexPWM/SCT output): the motor-control "snapshot the encoder position, synchronised to
 * the PWM carrier" path.  CTRL2 selects what the trigger does:
 *   - UPDHLD: coherently CAPTURE the position/revolution counters into the HOLD registers
 *     (UPOS/LPOS/REV -> UPOSH/LPOSH/REVH), so a FOC loop reads a consistent {position,
 *     revolution} snapshot taken at one instant instead of across three racing reads.
 *   - UPDPOS: CLEAR the position/revolution counters (a timed re-zero).
 * Both may be set; on silicon UPDHLD captures before UPDPOS clears, so the snapshot holds
 * the pre-clear value -- modelled in that order.
 *
 * ⚠ Scope, stated: the position counters themselves are guest-writable storage (an encoder
 * initialises them), but there is NO quadrature/encoder signal source in emulation, so they
 * do not ADVANCE on their own -- the PHASEA/PHASEB decode is the input seam.  What is
 * modelled is the trigger's CAPTURE/CLEAR of whatever position the guest/encoder set, which
 * is the entire function of QDCn_TRIG.
 */
static void mcxn_qdc_hw_trigger(void *opaque, int n, int level)
{
    MCXNQDCState *s = MCXN_QDC(opaque);
    uint16_t ctrl2 = qdc_ld16(s, QDC_CTRL2);

    if (!level) {
        return;                          /* a trigger is an edge, not a level */
    }
    if (ctrl2 & QDC_CTRL2_UPDHLD) {
        qdc_st16(s, QDC_UPOSH, qdc_ld16(s, QDC_UPOS));
        qdc_st16(s, QDC_LPOSH, qdc_ld16(s, QDC_LPOS));
        qdc_st16(s, QDC_REVH,  qdc_ld16(s, QDC_REV));
    }
    if (ctrl2 & QDC_CTRL2_UPDPOS) {
        qdc_st16(s, QDC_UPOS, 0);
        qdc_st16(s, QDC_LPOS, 0);
        qdc_st16(s, QDC_REV,  0);
    }
}

static const MemoryRegionOps mcxn_qdc_ops = {
    .read = mcxn_qdc_read,
    .write = mcxn_qdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * QDC (quadrature decoder) reset values, from the RM.
 *
 * ⭐ POSDPER IS THE POSITION-DIFFERENCE *PERIOD* COUNTER -- THE CLOCKS BETWEEN ENCODER
 *    EDGES -- AND A SPEED OBSERVER DIVIDES BY IT.
 *
 * Silicon resets it (and POSDPERBFR/POSDPERH/LASTEDGE/LASTEDGEH/UCOMP/LCOMP) to 0xFFFF:
 * the MAXIMUM period -- "no edge has been seen; the shaft is not turning".  We reset them
 * to ZERO, and on a PERIOD register zero does not mean "nothing":
 *
 *     ⭐ ZERO CLOCKS BETWEEN EDGES MEANS *INFINITE SPEED*.
 *
 * A FOC speed loop reading our reset value computes a divide-by-zero, or an infinite rotor
 * velocity, BEFORE THE MOTOR HAS MOVED AT ALL.
 *
 *     ⭐ THE DANGEROUS ZEROS ARE THE ONES WHERE ZERO IS A LEGAL, MEANINGFUL, CATASTROPHIC
 *        VALUE -- not the ones where it is merely wrong.
 *
 * ⚠ AND THE GATE COULD NOT SEE ANY OF IT: these are 16-BIT registers, and the extractor
 * kept only 32-bit ones.  rt1180emulator hit the identical 32-bit blindness, and HIS
 * refusal pile contained the eFlexPWM DEAD-TIME counters -- 0x07FF on silicon, ZERO in his
 * model.  Zero dead time is A DIRECT SHORT ACROSS THE DC BUS THROUGH BOTH TRANSISTORS OF
 * AN INVERTER LEG.  Every one of his PWM tests was green.
 */
static const struct { uint16_t off; uint8_t width; uint32_t val; } rst_tbl[] = {
    { 0x024, 16, 0x0000FFFFu },   /* UCOMP */
    { 0x026, 16, 0x0000FFFFu },   /* LCOMP */
    { 0x028, 16, 0x0000FFFFu },   /* LASTEDGE */
    { 0x02A, 16, 0x0000FFFFu },   /* LASTEDGEH */
    { 0x02C, 16, 0x0000FFFFu },   /* POSDPER */
    { 0x02E, 16, 0x0000FFFFu },   /* POSDPERBFR */
    { 0x030, 16, 0x0000FFFFu },   /* POSDPERH */
};

/* regs[] is a BYTE array here, and these registers are 8/16 bits.  Write little-endian
 * bytes -- a word store would clobber the neighbouring register that shares the word.
 * (I did exactly that on the first pass and the reset gate caught the regression on the
 * very next run.) */
static void mcxn_qdc_set_reset(uint8_t *regs, uint16_t off, uint8_t width, uint32_t val)
{
    int i;

    for (i = 0; i < width / 8; i++) {
        regs[off + i] = (val >> (8 * i)) & 0xFF;
    }
}

static void mcxn_qdc_reset(DeviceState *dev)
{
    MCXNQDCState *s = MCXN_QDC(dev);
    int rst_i;

    memset(s->regs, 0, sizeof(s->regs));
    for (rst_i = 0; rst_i < (int)ARRAY_SIZE(rst_tbl); rst_i++) {
        mcxn_qdc_set_reset(s->regs, rst_tbl[rst_i].off, rst_tbl[rst_i].width, rst_tbl[rst_i].val);
    }
}

static void mcxn_qdc_realize(DeviceState *dev, Error **errp)
{
    MCXNQDCState *s = MCXN_QDC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_qdc_ops, s,
                          TYPE_MCXN_QDC, MCXN_QDC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    /* Hardware trigger routed in by INPUTMUX from QDCn_TRIG (position capture/clear). */
    qdev_init_gpio_in_named(dev, mcxn_qdc_hw_trigger, "trigger", 1);
}

static const VMStateDescription vmstate_mcxn_qdc = {
    .name = TYPE_MCXN_QDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MCXNQDCState, MCXN_QDC_SIZE),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_qdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_qdc_realize;
    device_class_set_legacy_reset(dc, mcxn_qdc_reset);
    dc->vmsd = &vmstate_mcxn_qdc;
}

static const TypeInfo mcxn_qdc_types[] = {
    {
        .name          = TYPE_MCXN_QDC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNQDCState),
        .class_init    = mcxn_qdc_class_init,
    },
};

DEFINE_TYPES(mcxn_qdc_types)
