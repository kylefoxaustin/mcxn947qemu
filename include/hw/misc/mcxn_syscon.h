/*
 * NXP MCX N SYSCON — secondary-core (CPU1) boot control (partial model)
 *
 * Models the SYSCON CPUCTRL/CPBOOT registers that the primary Cortex-M33
 * (cpu0) uses to release the secondary Cortex-M33 (cpu1) from reset.  The rest
 * of the 4 KiB SYSCON window is backed permissively so early clock/reset init
 * does not fault; unmodelled accesses are logged.  Offsets and bit fields are
 * from the MCXN947 CMSIS header (SYSCON: CPUCTRL @0x800, CPBOOT @0x804).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SYSCON_H
#define HW_MISC_MCXN_SYSCON_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "target/arm/cpu-qom.h"

#define TYPE_MCXN_SYSCON "mcxn-syscon"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSysconState, MCXN_SYSCON)

#define MCXN_SYSCON_SIZE 0x1000

struct MCXNSysconState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_SYSCON_SIZE / 4]; /* permissive backing for other regs */
    uint32_t cpuctrl;                    /* CPU Control            (off 0x800) */
    uint32_t cpboot;                     /* Coprocessor Boot Addr  (off 0x804) */
    bool     cpu1_running;

    ARMCPU  *cpu1;   /* link: secondary core, released via CPUCTRL/CPBOOT */
};

#endif /* HW_MISC_MCXN_SYSCON_H */
