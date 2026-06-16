/*
 * NXP MCX N uSDHC (Ultra Secured Digital Host Controller) — bring-up model.
 *
 * Backs the uSDHC register file (CMSIS USDHC_Type) with enough present-state
 * and interrupt-status semantics for the SD/MMC init state machine to advance:
 * the command/data lines read idle, soft-reset bits self-clear, and issuing a
 * command raises command-complete.  Offsets/bits from the MCXN947 CMSIS header
 * (USDHC_Type).  The SoC supplies the base and IRQ.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_USDHC_H
#define HW_MISC_MCXN_USDHC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_USDHC "mcxn-usdhc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNUSDHCState, MCXN_USDHC)

/* CMSIS USDHC_Type spans up to TUNING_CTRL @0xCC; round the window to 0x1000. */
#define MCXN_USDHC_SIZE 0x1000

struct MCXNUSDHCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_USDHC_SIZE / 4];
};

#endif /* HW_MISC_MCXN_USDHC_H */
