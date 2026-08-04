/*
 * NXP MCX N eDMA (enhanced DMA) — functional model.
 *
 * 16 channels, each with a Transfer Control Descriptor (TCD).  Writing
 * TCD_CSR.START software-triggers the channel: the model performs the minor
 * loop (NBYTES, with SOFF/DOFF strides and SSIZE/DSIZE element width) CITER
 * times across the system address space, then applies SLAST/DLAST, sets
 * CH_CSR.DONE + CH_INT and, if TCD_CSR.INTMAJOR is set, raises the channel
 * IRQ.  Register layout from the MCXN947 CMSIS header (DMA_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_DMA_MCXN_EDMA_H
#define HW_DMA_MCXN_EDMA_H

#include "hw/core/sysbus.h"
#include "qemu/main-loop.h"
#include "qom/object.h"

#define TYPE_MCXN_EDMA "mcxn-edma"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNEDMAState, MCXN_EDMA)

#define MCXN_EDMA_CHANNELS 16

/*
 * MCX N DMA request-mux sources (CMSIS dma_request_source_t).  Peripherals
 * drive one GPIO input per source; a channel consumes the one its CH_MUX[SRC]
 * selects.  Highest source in the enum is 121, so 128 covers the 7-bit field.
 */
/*
 * Widest single source/destination transfer the engine will do (ATTR
 * SSIZE/DSIZE encode up to 64-byte bursts on MCX N).  A TCD asking for more is
 * CH_ES[NCE].
 */
#define MCXN_EDMA_MAX_XFER     64

#define MCXN_EDMA_REQ_SOURCES  128
#define CH_MUX_SRC_MASK        0x7Fu    /* CMSIS DMA_CH_MUX_SRC_MASK */
/*
 * backstop: a peripheral that never drops its request must not wedge the
 * machine
 */
#define MCXN_EDMA_MAX_LOOPS    0x100000

/* Request-mux source numbers used by the SoC wiring (CMSIS-exact). */
#define MCXN_DMA_REQ_FLEXSPI0_RX   1
#define MCXN_DMA_REQ_FLEXSPI0_TX   2
#define MCXN_DMA_REQ_CTIMER0_M0    7    /* CTIMER{k} M0 = 7 + 2k, M1 = 8 + 2k */
#define MCXN_DMA_REQ_CTIMER0_M1    8
/* SCT0 event -> DMA request 0 (DMAREQ0) */
#define MCXN_DMA_REQ_SCT0_DMA0     19
/* SCT0 event -> DMA request 1 (DMAREQ1) */
#define MCXN_DMA_REQ_SCT0_DMA1     20
/* HsCmp{n} DMA request = 28 + n (CMP0/1/2) */
#define MCXN_DMA_REQ_HSCMP0        28
/* PINT INT{n} DMA request = 3 + n, n=0..3 */
#define MCXN_DMA_REQ_PINT0         3
/* MICFIL0 (PDM) FIFO request (a level) */
#define MCXN_DMA_REQ_MICFIL0       18
#define MCXN_DMA_REQ_FLEXPWM0_CAP0 39   /* FlexPWM0 SM0 capture0 request */
#define MCXN_DMA_REQ_FLEXPWM1_CAP0 47   /* FlexPWM1 SM0 capture0 request */
/* FlexPWM0 SM0 value-register reload request */
#define MCXN_DMA_REQ_FLEXPWM0_VAL0 43
/* FlexPWM1 SM0 value-register reload request */
#define MCXN_DMA_REQ_FLEXPWM1_VAL0 51
#define MCXN_DMA_REQ_ADC0_FIFO_A   21
#define MCXN_DMA_REQ_ADC0_FIFO_B   22
#define MCXN_DMA_REQ_DAC0_FIFO     25
#define MCXN_DMA_REQ_DAC1_FIFO     26
#define MCXN_DMA_REQ_DAC2_FIFO     27
#define MCXN_DMA_REQ_LPFLEXCOMM0_RX 69   /* LpFlexcomm{n} Rx = 69 + 2n */
#define MCXN_DMA_REQ_LPFLEXCOMM0_TX 70   /* LpFlexcomm{n} Tx = 70 + 2n */
#define MCXN_DMA_REQ_SAI0_RX       99
#define MCXN_DMA_REQ_SAI0_TX       100
#define MCXN_DMA_REQ_SAI1_RX       101
#define MCXN_DMA_REQ_SAI1_TX       102
/* SINC0 ipd_req_sinc[n] = 103 + n, n=0..4 */
#define MCXN_DMA_REQ_SINC0_CH0     103

typedef struct MCXNEDMAChan {
    uint32_t csr, es, intr, sbr, pri, mux;
    uint32_t tcd_saddr, tcd_slast, tcd_daddr, tcd_dlast;
    uint32_t tcd_nbytes;
    uint16_t tcd_soff, tcd_attr, tcd_doff, tcd_citer, tcd_csr, tcd_biter;
    /* a scatter/gather TCD is loaded and awaiting resume */
    bool     sg_pending;
} MCXNEDMAChan;

struct MCXNEDMAState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq[MCXN_EDMA_CHANNELS];

    uint32_t mp_csr;
    uint32_t mp_es;
    uint32_t ch_grpri[MCXN_EDMA_CHANNELS];
    MCXNEDMAChan ch[MCXN_EDMA_CHANNELS];

    /*
     * Hardware request lines, one per request-mux source.  Latched so that a
     * peripheral re-asserting from inside a transfer is seen by the drain loop
     * rather than re-entering it.
     */
    bool req_level[MCXN_EDMA_REQ_SOURCES];

    /*
     * EDGE (pulse) sources.  A FIFO source holds its request high until the
     * FIFO drains, and the drain loop keys off that level -- one minor loop per
     * pass until the peripheral itself drops the line (SAI/FlexSPI/SINC), or
     * until the DMA writes back into the peripheral and it re-evaluates
     * (FlexPWM VALx).  But a TIMER-MATCH source (CTIMER, SCT) is an
     * instantaneous EVENT: it fires once, must move exactly ONE minor loop, and
     * there is no FIFO to re-check and no write-back to hook a deassert on.
     * For those sources the engine auto-acks -- it clears req_level after
     * servicing a single minor loop, so one pulse = one minor loop.  Latched
     * per source (a match line is always a pulse line).
     */
    bool req_edge[MCXN_EDMA_REQ_SOURCES];

    /*
     * INPUTMUX gates EVERY peripheral DMA request:
     * INPUTMUX_DMAn_REQ_ENABLE0..3, one bit per request source, reset
     * FFFF_FFFF/FFFF_FFFF/FFFF_FFFF/03FF_FFFF (i.e. all 122 lines ENABLED out
     * of reset).  RM: "0: DMA request to DMA0 and response from DMA0 are
     * blocked.  1: DMA request and response are enabled."
     *
     * ⚠ This gate did not exist, so our request lines were UNGATED.  That is
     * not a harmless simplification -- it is the silent-wrong-answer class
     * INVERTED: the model was MORE PERMISSIVE THAN THE SILICON, so a developer
     * who forgot the enable got a working transfer here and a dead one on the
     * board.  A model that is too forgiving does not fail safe; IT SHIPS THE
     * BUG TO THE HARDWARE.
     *
     * Defaults to all-enabled so a machine that never writes INPUTMUX behaves
     * exactly as silicon does out of reset.
     */
    bool req_enabled[MCXN_EDMA_REQ_SOURCES];

    /*
     * Which eDMA this is.  CH_SBR[MID] -- the BUS MASTER ID -- is PER-INSTANCE:
     * the RM gives DMA0 a reset of 6 and DMA1 a reset of 7.
     */
    uint8_t dma_id;

    /*
     * Requests are serviced from a bottom half: a peripheral raises its line
     * from inside its own MMIO write, and writing back into it on that call
     * stack is a re-entrant access that QEMU's guard silently DROPS.
     */
    QEMUBH *bh;

    /* Guard against a channel-link loop (A links B links A). */
    bool in_link;
};

#endif /* HW_DMA_MCXN_EDMA_H */
