/*
 * NXP MCX N BSP32 (CoolFlux BSP32 coprocessor bus block) — bring-up model.
 *
 * Faithful register file for the BSP32 coprocessor control block (memory offset
 * registers, interrupt registers and IVT registers).  Offsets from the MCXN947
 * CMSIS header (BSP32_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_BSP32_H
#define HW_MISC_MCXN_BSP32_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_BSP32 "mcxn-bsp32"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNBSP32State, MCXN_BSP32)

#define MCXN_BSP32_SIZE 0x1000

struct MCXNBSP32State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_BSP32_SIZE / 4];
};

#endif /* HW_MISC_MCXN_BSP32_H */
