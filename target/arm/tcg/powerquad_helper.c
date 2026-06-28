/*
 * NXP MCX-N PowerQuad custom coprocessor (CP0) — scalar math helpers.
 *
 * The PowerQuad exposes its scalar transcendentals and division through a
 * custom Arm coprocessor (CP_PQ = p0), driven with MCR/MCRR and read back with
 * MRC.  QEMU's Cortex-M33 does not implement CP0, so without this these ops
 * take a NOCP UsageFault -> HardFault (worse than a silent wrong answer).  When
 * ARM_FEATURE_POWERQUAD is set (only the MCXN947 SoC's M33s, via the
 * "powerquad" CPU property) the gated decode in translate-m-nocp.c routes the
 * ops here instead.
 *
 * Protocol (from the MCUXpresso SDK fsl_powerquad.h):
 *   - scalar f(x):  MCR p0, <opc1=func>, Rt=x, CRn=<(comp<<1)|fmt>, CRm=c0,
 *                   opc2=<machine: TRANS=0/TRIG=1 (+4 = fixed variants)>
 *   - division:     MCRR p0, <opc1=(comp<<1)|fmt>, {Rt=lo=denominator,
 *                   Rt2=hi=numerator}, CRm=PQ_DIV(6)
 *   - read result:  MRC p0, <opc1=0 (mult/result)>, CRn=<(comp<<1)|fmt> -> Rt
 *
 * Only the float32 path is computed (the dominant path — CMSIS-DSP-on-PowerQuad
 * and the direct PQ_*F32 API all use float32).  The fixed-point variants
 * (fmt=PQ_FIXEDPT) are deliberately NOT routed here: the gated decode lets them
 * fall through to the honest NOCP fault rather than risk a silently-wrong
 * Q-format result.  Results use host single-precision libm; the real PowerQuad
 * is an approximation engine (not IEEE-exact), so bit-exactness to silicon is
 * neither expected nor required — "the guest's math comes out right" is.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "cpu.h"
#include "helper.h"

/* CP0 scalar opcodes / machine selectors (fsl_powerquad.h). */
#define PQ_INV       0u
#define PQ_LN        1u
#define PQ_SQRT      2u
#define PQ_INVSQRT   3u
#define PQ_ETOX      4u
#define PQ_ETONX     5u
#define PQ_DIV       6u

#define PQ_SIN       0u
#define PQ_COS       1u

#define PQ_MACHINE_TRANS  0u
#define PQ_MACHINE_TRIG   1u

/* CRn encodes (comp << 1) | fmt; fixed-point is filtered out by the decode. */
static inline unsigned pq_comp(uint32_t crn) { return (crn >> 1) & 1u; }

static inline uint32_t pq_f2u(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static inline float pq_u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static float pq_scalar(uint32_t machine, uint32_t opc1, float x)
{
    if (machine == PQ_MACHINE_TRIG) {
        switch (opc1) {
        case PQ_SIN: return sinf(x);
        case PQ_COS: return cosf(x);
        default:     return 0.0f;
        }
    }
    /* PQ_MACHINE_TRANS */
    switch (opc1) {
    case PQ_INV:     return 1.0f / x;
    case PQ_LN:      return logf(x);
    case PQ_SQRT:    return sqrtf(x);
    case PQ_INVSQRT: return 1.0f / sqrtf(x);
    case PQ_ETOX:    return expf(x);
    case PQ_ETONX:   return expf(-x);
    default:         return 0.0f;
    }
}

/* MCR p0: compute a scalar transcendental and latch it in COMP[comp]. */
void HELPER(powerquad_mcr)(CPUARMState *env, uint32_t input, uint32_t opc1,
                           uint32_t crn, uint32_t opc2)
{
    unsigned comp = pq_comp(crn);
    float r = pq_scalar(opc2, opc1, pq_u2f(input));

    env->powerquad.comp[comp] = pq_f2u(r);
}

/* MCRR p0: division.  lo = denominator (x2), hi = numerator (x1). */
void HELPER(powerquad_mcrr)(CPUARMState *env, uint32_t lo, uint32_t hi,
                            uint32_t opc1, uint32_t crm)
{
    unsigned comp = pq_comp(opc1);   /* MCRR carries (comp<<1)|fmt in opc1 */
    float r = 0.0f;

    if (crm == PQ_DIV) {
        r = pq_u2f(hi) / pq_u2f(lo);
    }
    env->powerquad.comp[comp] = pq_f2u(r);
}

/* MRC p0: read back the latched COMP[comp] result (raw float32 bits). */
uint32_t HELPER(powerquad_mrc)(CPUARMState *env, uint32_t opc1, uint32_t crn)
{
    return env->powerquad.comp[pq_comp(crn)];
}
