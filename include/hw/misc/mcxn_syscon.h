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
#include "hw/core/clock.h"
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

    /*
     * THE CLOCK TREE, as far as it is modelled.
     *
     * SYSCON's *CLKSEL registers do not merely record a choice -- THEY DECIDE THE
     * RATE THE PERIPHERAL ACTUALLY RUNS AT.  Until now nothing consumed them, and the
     * OSTIMER simply hardcoded 1 MHz:
     *
     *     return hz ? hz : OSTIMER_HZ;     -- and its Clock was NEVER CONNECTED
     *
     * so `hz` was always 0 and THE FALLBACK WAS THE CAMOUFLAGE.  A guest that selected
     * the 16 kHz source got a timer running at 1 MHz -- SIXTY-TWO TIMES TOO FAST --
     * and nothing said a word.  The model had the right structure (a Clock input) and
     * a default that made the missing wiring invisible.
     */
    Clock *ostimer_clk;
    uint32_t cpuctrl;                    /* CPU Control            (off 0x800) */
    uint32_t cpboot;                     /* Coprocessor Boot Addr  (off 0x804) */
    bool     cpu1_running;

    ARMCPU  *cpu1;   /* link: secondary core, released via CPUCTRL/CPBOOT */
};

#endif /* HW_MISC_MCXN_SYSCON_H */
