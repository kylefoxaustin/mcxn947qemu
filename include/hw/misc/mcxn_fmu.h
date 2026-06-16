/*
 * NXP MCX N FMU (Flash Management Unit) — functional flash controller.
 *
 * Models the command interface: FSTAT (with the CCIF command-complete flag),
 * FCNFG, FCTRL and the 8-word FCCOB command object.  Commands launched by
 * clearing CCIF complete instantly (CCIF re-asserts); Erase All / Erase Sector
 * zero the backing flash to the erased state (0xFF) and the verify (Read 1s)
 * commands check it.  Program data is written directly to the flash address by
 * firmware, which lands in the RAM-backed flash, so Program completes as a
 * no-op here.  Offsets/bits from the MCXN947 CMSIS header (FMU_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FMU_H
#define HW_MISC_MCXN_FMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_MCXN_FMU "mcxn-fmu"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNFMUState, MCXN_FMU)

struct MCXNFMUState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;

    uint32_t fstat;
    uint32_t fcnfg;
    uint32_t fctrl;
    uint32_t fccob[8];

    /* Backing flash, set by the SoC so erase/verify can reach it. */
    MemoryRegion *flash;
    uint64_t      flash_size;
};

#endif /* HW_MISC_MCXN_FMU_H */
