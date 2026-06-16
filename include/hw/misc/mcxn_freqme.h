/*
 * NXP MCX N FREQME (Frequency Measurement) — bring-up model.
 *
 * Measures a target clock against a scaled reference clock.  Firmware starts a
 * measurement by writing CTRL_W[MEASURE_IN_PROGRESS]=1 then polls until the bit
 * clears (read via CTRL_R, the read alias of the same 0x0 offset) and reads the
 * RESULT.  This model completes measurements instantly.  Offsets from the
 * MCXN947 CMSIS header (FREQME_Type, base 0x40011000).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FREQME_H
#define HW_MISC_MCXN_FREQME_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_FREQME "mcxn-freqme"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNFreqmeState, MCXN_FREQME)

#define MCXN_FREQME_SIZE 0x1000

struct MCXNFreqmeState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t ctrl;      /* CTRL_W config (offset 0x0, write) */
    uint32_t ctrlstat;  /* CTRLSTAT (offset 0x4): config mirror + W1C status */
    uint32_t result;    /* CTRL_R[RESULT] last measurement result */
    uint32_t min;       /* MIN (offset 0x8) */
    uint32_t max;       /* MAX (offset 0xC) */
};

#endif /* HW_MISC_MCXN_FREQME_H */
