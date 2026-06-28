/*
 * NXP MCX N FlexSPI (Flexible Serial Peripheral Interface) — bring-up model.
 *
 * Backs the FlexSPI register file (CMSIS FLEXSPI_Type) with enough controller
 * status for firmware to init the external-flash interface: the module
 * software-reset self-clears, the controller reports idle, and launching an IP
 * command reports it done.  Offsets/bits from the MCXN947 CMSIS header
 * (FLEXSPI_Type).  The SoC supplies the base and IRQ.
 *
 * In addition to the register file (MMIO region 0), the device exposes the
 * AHB-mapped external NOR as RAM-backed memory (MMIO region 1).  Mapping it
 * into the FlexSPI0 AHB window (NS 0x8000_0000 / secure 0x9000_0000) makes the
 * window real executable memory, so code linked there runs in place (XIP) and
 * AHB reads return the loaded flash contents instead of faulting.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FLEXSPI_H
#define HW_MISC_MCXN_FLEXSPI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_FLEXSPI "mcxn-flexspi"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNFlexSPIState, MCXN_FLEXSPI)

/* CMSIS FLEXSPI_Type spans up to 0x5F4; round the window to 0x1000. */
#define MCXN_FLEXSPI_SIZE 0x1000

struct MCXNFlexSPIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;        /* MMIO region 0: CMSIS register file          */
    MemoryRegion nor;          /* MMIO region 1: AHB-mapped external NOR (XIP) */
    qemu_irq irq;
    uint32_t regs[MCXN_FLEXSPI_SIZE / 4];
    uint64_t flash_size;       /* size of the AHB NOR window ("flash-size")    */
};

#endif /* HW_MISC_MCXN_FLEXSPI_H */
