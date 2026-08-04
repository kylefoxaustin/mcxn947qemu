/*
 * NXP MCX N SMARTDMA (programmable DMA coprocessor) - bring-up model.
 *
 * The SMARTDMA is an "EZH" programmable engine started via the CTRL.START bit.
 * This model backs the control/boot registers permissively and keeps the engine
 * reading not-busy so firmware that boots it and polls never stalls.  Offsets
 * and bits from the MCXN947 CMSIS header (SMARTDMA_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SMARTDMA_H
#define HW_MISC_MCXN_SMARTDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_SMARTDMA "mcxn-smartdma"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSmartDMAState, MCXN_SMARTDMA)

#define MCXN_SMARTDMA_SIZE 0x1000

struct MCXNSmartDMAState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_SMARTDMA_SIZE / 4];

    /* Fidelity flag-at-operator: the SmartDMA "EZH" core executes a firmware
     * program we do NOT run, so a CTRL.START is acked but does no work (silent
     * no-compute).  Counts program starts acked-but-not-executed; exposed via
     * the read-only QOM properties "compute-modelled" (false) and
     * "programs-started" so the farm control-plane can detect a guest trusting
     * an accelerator that isn't computing — without hanging it. */
    uint32_t programs_started;

    /* Boot decode, for an INFORMATIVE honest-fault (still no compute).  On a
     * keyed CTRL boot we recover which documented operation was requested — the
     * apiIndex from the firmware jump table at SRAMX, plus the raw entry, the
     * ARM2EZH-carried pParam and its 2-bit mask.  Exposed via QOM so the farm
     * control-plane sees exactly which op a guest asked an un-run engine for.
     * last_apiindex == 0xFFFFFFFF means "not recovered" (unrecognised firmware
     * or non-standard load address). */
    uint32_t last_bootadr;
    uint32_t last_apiindex;
    uint32_t last_pparam;
    uint32_t last_mask;
};

#endif /* HW_MISC_MCXN_SMARTDMA_H */
