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
#include "qom/object.h"

#define TYPE_MCXN_EDMA "mcxn-edma"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNEDMAState, MCXN_EDMA)

#define MCXN_EDMA_CHANNELS 16

typedef struct MCXNEDMAChan {
    uint32_t csr, es, intr, sbr, pri, mux;
    uint32_t tcd_saddr, tcd_slast, tcd_daddr, tcd_dlast;
    uint32_t tcd_nbytes;
    uint16_t tcd_soff, tcd_attr, tcd_doff, tcd_citer, tcd_csr, tcd_biter;
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
};

#endif /* HW_DMA_MCXN_EDMA_H */
