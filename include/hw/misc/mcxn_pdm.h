/*
 * NXP MCX N PDM / MICFIL (digital microphone interface) — honest model.
 *
 * MICFIL's input is a physical PDM microphone bitstream on the PDM_DATAn pins.
 * Unlike the SINC, the RM gives it no register-fed bitstream path, so there is
 * nothing for it to decimate in emulation without an operator-supplied audio
 * source.  That makes the *honest* model an explicitly-empty one — and, crucially,
 * a LOUD one:
 *
 *   - Enabling the filter (CTRL_1[PDMIEN]) with no audio source logs LOG_UNIMP
 *     and produces no samples.
 *   - The per-channel FIFOs report genuinely empty: STAT[CHnF] (watermark
 *     reached) never sets, so an interrupt/DMA-driven capture blocks rather than
 *     appearing to succeed.
 *   - Reading DATACHn with an empty FIFO sets FIFO_STAT[FIFOUNDn].  A polling
 *     capture therefore gets a detectable underflow instead of a plausible zero.
 *
 * That last point is the whole design.  The previous bring-up model returned
 * DATACHn = 0 forever with no flags, which is a mute microphone that looks
 * exactly like a working one: a keyword-spotter or voice-activity demo records a
 * buffer of perfect digital silence and reports success. A silent zero-stream
 * firmware cannot distinguish from real audio is precisely the top-tier bug this
 * project exists to kill.
 *
 * The FIFO machinery below is real, and an operator-driven PCM source now feeds
 * it: the "mic-input" QOM property pushes a 24-bit sample into channel 0's FIFO
 * (the single-microphone case), lighting up the watermark / overflow / IRQ paths
 * and — when CTRL_1[DISEL] selects DMA — the MICFIL FIFO request (CMSIS source
 * 18), a FIFO LEVEL (like the SAI), so an operator-fed capture drains over eDMA.
 *
 * Geometry from PARAM (RM §76.7.14, reset 0x0000_0742): NPAIR = 2 (4 channels),
 * FIFO_PTRWID = 4 (16-deep per-channel FIFO), 24-bit filter output.
 * Offsets/bits from the MCXN947 CMSIS header (PDM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PDM_H
#define HW_MISC_MCXN_PDM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PDM "mcxn-pdm"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPDMState, MCXN_PDM)

#define MCXN_PDM_SIZE       0x1000
#define MCXN_PDM_NUM_CH     4     /* PARAM[NPAIR] = 2 pairs = 4 channels */
#define MCXN_PDM_FIFO_DEPTH 16    /* PARAM[FIFO_PTRWID] = 4 -> 2^4 */

struct MCXNPDMState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;        /* PDM_EVENT_IRQn = 48 */
    qemu_irq     dma_req;    /* MICFIL0 FIFO request (CMSIS source 18), a FIFO level */
    bool         dma_lvl;    /* current level of dma_req, so we fire only on change */

    uint32_t regs[MCXN_PDM_SIZE / 4];

    /* Per-channel output FIFOs, holding DATACHn as the guest reads it (24-bit
     * PCM).  Empty unless an audio source feeds them; see the header comment. */
    uint32_t fifo[MCXN_PDM_NUM_CH][MCXN_PDM_FIFO_DEPTH];
    uint32_t fifo_count[MCXN_PDM_NUM_CH];

    bool warned_no_source;
};

#endif /* HW_MISC_MCXN_PDM_H */
