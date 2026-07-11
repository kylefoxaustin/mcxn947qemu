/*
 * NXP MCX N SINC (sigma-delta / sinc filter) — functional model.
 *
 * The SINC decimates a 1-bit sigma-delta modulator bitstream through a
 * cascaded-integrator-comb (CIC) filter into a 24-bit result.  Crucially, the
 * RM specifies a *register-fed* bitstream source (RM rev 7 §47.3.2.3.7 "PM" and
 * §47.3.2.3.8 "SM"), so the whole block can be modelled to full fidelity with
 * no analog source and nothing invented:
 *
 *   CnCFR[IBFMT] = 10b (PM)  writing CnMPDATA feeds its low 16 bits to the CIC
 *   CnCFR[IBFMT] = 11b (SM)  writing CnMPDATA shifts all 32 bits into the CIC
 *
 * We implement the RM's transfer function exactly (§47.3.2.7.1.1):
 *
 *     H(z) = ( (1 - z^-OSR) / (1 - z^-1) ) ^ ORD
 *     OSR  = CnDR[PFOSR] + 1        ORD = CnDR[PFORD]
 *
 * and the FastSinc variant when PFORD == 0 (§47.3.2.7.1.2):
 *
 *     H(z) = ( (1 - z^-OSR) / (1 - z^-1) ) ^ 2  *  ( 1 + z^-2*OSR )
 *
 * i.e. integrators at the input (bitstream) rate, decimate by OSR, combs at the
 * output rate — so a result is arithmetically checkable against a reference
 * filter rather than being a plausible-looking constant.
 *
 * The external bitstream sources (MBIT/INP pins) have no source in emulation.
 * Those are LOUD, not silent: the model logs and produces no samples rather
 * than manufacturing an endless stream of zeros that firmware cannot tell apart
 * from real data.  (That silent-zero behaviour is what the previous bring-up
 * model did, and it is precisely the top-tier bug this project exists to kill:
 * in the RM's own headline use case — motor-control coil-current sensing — a
 * forever-zero SINC result reads as "0 amps on every phase", and the control
 * loop converges happily on a lie.)
 *
 * Offsets/bits/reset values from the MCXN947 CMSIS header (SINC_Type) and the
 * RM register tables (§47.7).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SINC_H
#define HW_MISC_MCXN_SINC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_SINC "mcxn-sinc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSINCState, MCXN_SINC)

#define MCXN_SINC_SIZE       0x1000
#define MCXN_SINC_NUM_CH     5     /* PARAMETER[FLT_NUM]   = 5 */
#define MCXN_SINC_FIFO_DEPTH 8     /* PARAMETER[FIFO_DEPTH] = 8 */
#define MCXN_SINC_MAX_ORD    4     /* CIC order: PFORD 1..3, FastSinc uses 2 */

typedef struct MCXNSINCChannel {
    /* CIC state.  Integrators run at the bitstream rate, combs at the
     * decimated (output) rate. */
    int64_t  integ[MCXN_SINC_MAX_ORD];
    int64_t  comb[MCXN_SINC_MAX_ORD];
    int64_t  fs_delay[2];      /* FastSinc's (1 + z^-2) at the output rate */
    uint32_t phase;            /* input bits since the last decimation      */

    bool     running;          /* a trigger has started conversion (PFCM)   */

    int32_t  fifo[MCXN_SINC_FIFO_DEPTH];
    uint32_t fifo_count;
    int32_t  last;             /* most recent result when the FIFO is off   */
    bool     have_last;
} MCXNSINCChannel;

struct MCXNSINCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;          /* SINC_FILTER_IRQn = 142 */

    uint32_t regs[MCXN_SINC_SIZE / 4];
    MCXNSINCChannel ch[MCXN_SINC_NUM_CH];

    bool warned_ext_source;    /* only log the "no bitstream source" note once */
};

#endif /* HW_MISC_MCXN_SINC_H */
