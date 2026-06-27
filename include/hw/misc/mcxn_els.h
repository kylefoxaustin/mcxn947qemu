/*
 * NXP MCX N ELS (EdgeLock Secure subsystem, S50) - bring-up model.
 *
 * Faithful register file for the ELS crypto subsystem with no real crypto.
 * The key bring-up concern is the ELS_STATUS BUSY bit that firmware polls
 * after issuing a command: it reads idle/done so any "wait for ELS" loop
 * completes.  Offsets from the MCXN947 CMSIS header (S50_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_ELS_H
#define HW_MISC_MCXN_ELS_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_ELS "mcxn-els"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNELSState, MCXN_ELS)

#define MCXN_ELS_SIZE 0x1000

struct MCXNELSState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_ELS_SIZE / 4];
    uint32_t rng_state;   /* PRNG backing the TRNG data output (PRNG_DATOUT) */
};

#endif /* HW_MISC_MCXN_ELS_H */
