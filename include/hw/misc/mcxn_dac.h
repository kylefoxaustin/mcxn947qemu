/*
 * NXP MCX N DAC (DAC0/DAC1 = 12-bit LPDAC, DAC2 = 14-bit HPDAC) — functional
 * output-FIFO model.
 *
 * The analog voltage is the only part of this block firmware cannot observe.
 * Everything else — FIFO occupancy, FULL/EMPTY/watermark, overflow, underflow,
 * the read/write pointers — is purely digital and fully specified, so it is
 * modelled for real (RM rev 7 §42.3 / §43.3):
 *
 *   - a DATA write pushes a sample (buffer mode: it *is* the output);
 *   - a trigger (TCR[SWTRG] or hardware) pops one sample to the output;
 *   - FULL/EMPTY/WM are computed from occupancy against FCR[WML];
 *   - writing a full FIFO drops the sample and sets FSR[OF] — the write pointer
 *     does not advance;
 *   - triggering an empty FIFO sets FSR[UF] and the output holds its last value.
 *
 * Why this matters (each was a silent-wrong-answer in the previous bring-up
 * model, which had no FIFO at all and reported FSR = EMPTY|WM forever):
 *
 *   - EMPTY auto-clears when DATA is written.  Hardwiring it high means a
 *     level-triggered EMPTY_IE interrupt never deasserts: the ISR re-enters for
 *     ever.  (The old DAC test had to disable IER inside its own handler to
 *     survive that — it was validating the model's fiction.)
 *   - Overflow was invisible: firmware could burst 64 samples into a 16-deep
 *     FIFO and see a happy, empty FIFO, while silicon drops 48 of them.
 *   - Underflow was invisible: a DMA-fed waveform that falls behind gets FSR[UF]
 *     and a frozen output on silicon; here it looked perfect.
 *   - `while (!(FSR & FULL)) DATA = *p++;` — a legitimate fill loop — never
 *     terminated.
 *
 * Per-instance geometry, from the CMSIS headers and RM §42.7.1.2/§43.7.1.2:
 *
 *              data width   FIFO depth   PARAM[FIFOSZ]   (depth = 2^(FIFOSZ+1))
 *   LPDAC 0/1  12-bit       16           3
 *   HPDAC 2    14-bit       32           4
 *
 * Not modelled, and LOUD about it (LOG_UNIMP on enable) rather than silently
 * wrong: the periodic-trigger generator (GCR[PTGEN] + PCR) and swing-back mode
 * (GCR[SWMD]).  See README "Known limitations".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_DAC_H
#define HW_MISC_MCXN_DAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_DAC "mcxn-dac"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNDACState, MCXN_DAC)

#define MCXN_DAC_SIZE      0x1000
#define MCXN_DAC_FIFO_MAX  32     /* HPDAC; LPDAC is 16 */

struct MCXNDACState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    /* DMA request line into the eDMA (DAC0/1/2 = mux source 25/26/27), driven
     * by the FIFO watermark/empty condition gated by DER. */
    qemu_irq dma_req;
    bool     dma_req_level;
    uint32_t regs[MCXN_DAC_SIZE / 4];

    /* "hpdac" property: DAC2 is the 14-bit part with the deeper FIFO. */
    bool     hpdac;

    uint16_t fifo[MCXN_DAC_FIFO_MAX];
    uint32_t wptr;      /* FPR[FIFO_WPT] */
    uint32_t rptr;      /* FPR[FIFO_RPT] */
    uint32_t count;     /* occupancy: disambiguates full from empty */
    uint32_t out;       /* the converted (analog) sample; holds on underflow */

    bool warned_ptg;
    bool warned_swmd;
};

#endif /* HW_MISC_MCXN_DAC_H */
