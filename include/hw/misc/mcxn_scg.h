/*
 * NXP MCX N SCG (System Clock Generator) — bring-up stub
 *
 * Enough of the SCG to get firmware clock-init past its "enable oscillator/PLL
 * then poll for valid/lock" loops.  Not a real clock tree: reads of the
 * oscillator/PLL control-status registers always report ready.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SCG_H
#define HW_MISC_MCXN_SCG_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_MCXN_SCG "mcxn-scg"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSCGState, MCXN_SCG)

#define MCXN_SCG_SIZE 0x1000

struct MCXNSCGState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_SCG_SIZE / 4];

    /*
     * THE SOURCE CLOCKS SCG ACTUALLY PRODUCES.  SYSCON muxes these to the
     * peripherals per its *CLKSEL registers.  Both are DERIVED from registers the
     * guest writes -- never asserted by us:
     *
     *   fro12m : 12 MHz, gated by SIRCCSR[SIRC_CLK_PERIPH_EN]
     *   frohf  : 0 if !FIRCCSR[FIRCEN]; 144 MHz if FIRCCFG[RANGE]; else 48 MHz
     *            (fsl_clock.c: CLOCK_GetFroHfFreq -- the SDK's own logic, mirrored)
     */
    Clock *fro12m;
    Clock *frohf;
};

#endif /* HW_MISC_MCXN_SCG_H */
