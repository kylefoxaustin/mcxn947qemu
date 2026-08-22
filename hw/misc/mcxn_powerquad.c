/*
 * NXP MCX N POWERQUAD (DSP math coprocessor) - bring-up model.  See header for
 * design notes.  Offsets and bits from the MCXN947 CMSIS header
 * (POWERQUAD_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_powerquad.h"
#include "hw/core/irq.h"
#include "exec/cpu-common.h"   /* cpu_physical_memory_read/write */
#include "migration/vmstate.h"
#include <math.h>

/* Register offsets (CMSIS POWERQUAD_Type). */
#define R_OUTBASE   0x00
#define R_OUTFORMAT 0x04
#define R_INABASE   0x10
#define R_INAFORMAT 0x14
#define R_INBBASE   0x18
#define R_INBFORMAT 0x1C
#define R_CONTROL   0x100  /* RW: opcode/machine launch; INST_BUSY in bit 31 */
#define R_LENGTH    0x104
#define R_CPPRE     0x108
#define R_MISC      0x10C
#define R_CURSORY   0x110
#define R_ERRSTAT   0x18C  /* error status (W1C) */
#define R_INTREN    0x190
#define R_EVENTEN   0x194
#define R_INTRSTAT  0x198  /* interrupt status (W1C) */

#define CONTROL_INST_BUSY  (1u << 31)  /* reads 0 (idle) in this model */
#define INTRSTAT_INTR_STAT (1u << 0)   /* completion interrupt status (W1C) */
#define INTREN_INTR_EN     (1u << 0)   /* completion interrupt enable */
#define ERRSTAT_MASK       0x1Fu     /* OVERFLOW/NAN/FIXEDOVERFLOW/UFLOW/BERR */
#define ERRSTAT_BUSERROR   0x10u       /* guest-visible "operation failed" */

/*
 * CONTROL: engine ("machine") in bits[6:4], opcode in bits[3:0]
 * (driver writes (CP_xxx << 4) | opcode).
 */
#define CTRL_OPCODE(c)   ((c) & 0xFu)
#define CTRL_MACHINE(c)  (((c) >> 4) & 0x7u)
#define CP_MTX  1u    /* matrix/vector engine */

/* Matrix/vector opcodes (fsl_powerquad.h). */
#define PQ_MTX_SCALE 1u
#define PQ_MTX_MULT  2u
#define PQ_MTX_ADD   3u
#define PQ_MTX_INV   4u
#define PQ_MTX_PROD  5u
#define PQ_MTX_SUB   7u
#define PQ_VEC_DOTP  9u
#define PQ_MTX_TRAN  10u

/*
 * Element type: format register bits[5:4]
 * (driver: format = (pre<<8)|(type<<4)|mf).
 */
#define PQ_TYPE(fmt)  (((fmt) >> 4) & 0x3u)
#define PQ_T_Q15   0u   /* int16 fixed Q15 */
#define PQ_T_Q31   1u   /* int32 fixed Q31 */
#define PQ_T_FLOAT 2u   /* IEEE-754 float32 */

/*
 * Read/write one element at base+i*stride as a double, per element type.
 * (Operands live in guest physical memory pointed to by the BASE registers.)
 */
static double pq_load(uint32_t base, uint32_t i, uint32_t type)
{
    switch (type) {
    case PQ_T_Q15: {
        int16_t v = 0;
        cpu_physical_memory_read(base + i * 2u, &v, 2);
        return (double)le16_to_cpu(v) / 32768.0;
    }
    case PQ_T_Q31: {
        int32_t v = 0;
        cpu_physical_memory_read(base + i * 4u, &v, 4);
        return (double)(int32_t)le32_to_cpu(v) / 2147483648.0;
    }
    default: {   /* PQ_T_FLOAT */
        uint32_t v = 0;
        float f;
        cpu_physical_memory_read(base + i * 4u, &v, 4);
        v = le32_to_cpu(v);
        memcpy(&f, &v, 4);
        return (double)f;
    }
    }
}

static void pq_store(uint32_t base, uint32_t i, uint32_t type, double d)
{
    switch (type) {
    case PQ_T_Q15: {
        double q = d * 32768.0;
        int32_t s = (int32_t)lrint(q);
        if (s > 32767) {
            s = 32767;
        } else if (s < -32768) {
            s = -32768;
        }
        uint16_t v = cpu_to_le16((int16_t)s);
        cpu_physical_memory_write(base + i * 2u, &v, 2);
        break;
    }
    case PQ_T_Q31: {
        double q = d * 2147483648.0;
        int64_t s = (int64_t)llrint(q);
        if (s > 2147483647) {
            s = 2147483647;
        } else if (s < -2147483648LL) {
            s = -2147483648LL;
        }
        uint32_t v = cpu_to_le32((uint32_t)(int32_t)s);
        cpu_physical_memory_write(base + i * 4u, &v, 4);
        break;
    }
    default: {   /* PQ_T_FLOAT */
        float f = (float)d;
        uint32_t v;
        memcpy(&v, &f, 4);
        v = cpu_to_le32(v);
        cpu_physical_memory_write(base + i * 4u, &v, 4);
        break;
    }
    }
}

static void mcxn_powerquad_update_irq(MCXNPowerQuadState *s)
{
    bool active = (s->regs[R_INTRSTAT >> 2] & INTRSTAT_INTR_STAT) &&
                  (s->regs[R_INTREN >> 2] & INTREN_INTR_EN);
    qemu_set_irq(s->irq, active);
}

/*
 * Execute a matrix/vector-engine (CP_MTX) instruction for real: read the input
 * operands from guest memory (INABASE/INBBASE), compute, write the result to
 * OUTBASE.  LENGTH packs the matrix dims as row|col<<8|mat2col<<16 (vector ops
 * use the raw count).  This makes PowerQuad return correct results instead of
 * leaving OUTBASE untouched (silent wrong answers).
 */
static void mcxn_powerquad_matrix(MCXNPowerQuadState *s, uint32_t opcode)
{
    uint32_t outb = s->regs[R_OUTBASE >> 2];
    uint32_t inab = s->regs[R_INABASE >> 2];
    uint32_t inbb = s->regs[R_INBBASE >> 2];
    uint32_t len  = s->regs[R_LENGTH >> 2];
    uint32_t at   = PQ_TYPE(s->regs[R_INAFORMAT >> 2]);
    uint32_t bt   = PQ_TYPE(s->regs[R_INBFORMAT >> 2]);
    uint32_t ot   = PQ_TYPE(s->regs[R_OUTFORMAT >> 2]);
    uint32_t r1 = len & 0xFFu, c1 = (len >> 8) & 0xFFu;
    uint32_t c2 = (len >> 16) & 0xFFu;
    uint32_t i, j, k;

    switch (opcode) {
    case PQ_MTX_MULT:
        /* C[r1 x c2] = A[r1 x c1] * B[c1 x c2], row-major. */
        for (i = 0; i < r1; i++) {
            for (j = 0; j < c2; j++) {
                double acc = 0.0;
                for (k = 0; k < c1; k++) {
                    acc += pq_load(inab, i * c1 + k, at) *
                           pq_load(inbb, k * c2 + j, bt);
                }
                pq_store(outb, i * c2 + j, ot, acc);
            }
        }
        break;
    case PQ_MTX_ADD:
    case PQ_MTX_SUB:
        for (i = 0; i < r1 * c1; i++) {
            double a = pq_load(inab, i, at), b = pq_load(inbb, i, bt);
            pq_store(outb, i, ot, opcode == PQ_MTX_ADD ? a + b : a - b);
        }
        break;
    case PQ_MTX_SCALE: {
        /* Scalar is the float32 bits in MISC (PQ_MatrixScale). */
        uint32_t mb = s->regs[R_MISC >> 2];
        float sf;
        memcpy(&sf, &mb, 4);
        for (i = 0; i < r1 * c1; i++) {
            pq_store(outb, i, ot, pq_load(inab, i, at) * (double)sf);
        }
        break;
    }
    case PQ_MTX_TRAN:
        /* C[c1 x r1] = A[r1 x c1]^T. */
        for (i = 0; i < r1; i++) {
            for (j = 0; j < c1; j++) {
                pq_store(outb, j * r1 + i, ot, pq_load(inab, i * c1 + j, at));
            }
        }
        break;
    case PQ_VEC_DOTP: {
        /*
         * Dot product of two length-N vectors -> scalar at OUTBASE.
         * Vector ops carry the raw count in LENGTH (cap to guard garbage).
         */
        uint32_t n = len > 0x10000u ? 0x10000u : len;
        double acc = 0.0;
        for (i = 0; i < n; i++) {
            acc += pq_load(inab, i, at) * pq_load(inbb, i, bt);
        }
        pq_store(outb, 0, ot, acc);
        break;
    }
    default:
        /*
         * INV (Gauss-Jordan) / PROD and the FFT/FIR engines are not computed.
         *
         * The result would be left STALE in the guest's output buffer, so
         * the guest must be TOLD — a host-side LOG_UNIMP is not enough,
         * because the firmware under test cannot see it and will read the
         * stale buffer as its DSP result.  Raise the engine's own error
         * flag (ERRSTAT[BUSERROR]), which fsl_powerquad checks, so the
         * operation reports as failed instead of quietly returning whatever
         * was in memory.
         */
        s->regs[R_ERRSTAT >> 2] |= ERRSTAT_BUSERROR;
        qemu_log_mask(LOG_UNIMP,
                      "%s: CP_MTX opcode %u NOT COMPUTED — failing it "
                      "via ERRSTAT[BUSERROR] rather than leaving a stale "
                      "result the guest would read as an answer\n",
                      __func__, opcode);
        break;
    }
}

static uint64_t mcxn_powerquad_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(opaque);
    uint32_t v = (off < MCXN_POWERQUAD_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case R_CONTROL:
        /*
         * Every instruction retires instantly: never report INST_BUSY so a
         * launch-then-poll loop completes on the first read.
         */
        return v & ~CONTROL_INST_BUSY;
    default:
        return v;
    }
}

static void mcxn_powerquad_write(void *opaque, hwaddr off,
                                 uint64_t value, unsigned size)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(opaque);
    uint32_t v = value;

    if (off >= MCXN_POWERQUAD_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_CONTROL:
        /*
         * Launch: execute the instruction for real (matrix/vector engine),
         * then retire instantly — clear INST_BUSY and raise the completion
         * interrupt.  (The scalar/vector transcendental ops use the ARM
         * CP0/CP1 coprocessor path, handled in the CPU core, not here.)
         */
        s->regs[off >> 2] = v & ~CONTROL_INST_BUSY;
        if (CTRL_MACHINE(v) == CP_MTX) {
            mcxn_powerquad_matrix(s, CTRL_OPCODE(v));
        }
        s->regs[R_INTRSTAT >> 2] |= INTRSTAT_INTR_STAT;
        mcxn_powerquad_update_irq(s);
        return;
    case R_ERRSTAT:
        /* Error flags are write-1-to-clear; no errors are ever generated. */
        s->regs[off >> 2] &= ~(v & ERRSTAT_MASK);
        return;
    case R_INTREN:
        s->regs[off >> 2] = v;
        mcxn_powerquad_update_irq(s);
        return;
    case R_INTRSTAT:
        /* Completion interrupt status is write-1-to-clear. */
        s->regs[off >> 2] &= ~(v & INTRSTAT_INTR_STAT);
        mcxn_powerquad_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_powerquad_ops = {
    .read = mcxn_powerquad_read,
    .write = mcxn_powerquad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_powerquad_reset(DeviceState *dev)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void mcxn_powerquad_realize(DeviceState *dev, Error **errp)
{
    MCXNPowerQuadState *s = MCXN_POWERQUAD(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_powerquad_ops, s,
                          TYPE_MCXN_POWERQUAD, MCXN_POWERQUAD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

/* Re-drive the IRQ line from restored register state after migration: the
 * output line is not part of vmstate, so a VM migrated with INTRSTAT & INTREN
 * asserted would otherwise land with the line low. */
static int mcxn_powerquad_post_load(void *opaque, int version_id)
{
    mcxn_powerquad_update_irq(MCXN_POWERQUAD(opaque));
    return 0;
}

static const VMStateDescription vmstate_mcxn_powerquad = {
    .name = TYPE_MCXN_POWERQUAD,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mcxn_powerquad_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPowerQuadState, MCXN_POWERQUAD_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_powerquad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_powerquad_realize;
    device_class_set_legacy_reset(dc, mcxn_powerquad_reset);
    dc->vmsd = &vmstate_mcxn_powerquad;
}

static const TypeInfo mcxn_powerquad_types[] = {
    {
        .name          = TYPE_MCXN_POWERQUAD,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPowerQuadState),
        .class_init    = mcxn_powerquad_class_init,
    },
};

DEFINE_TYPES(mcxn_powerquad_types)
