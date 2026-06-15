/*
 * NXP MCX N GPIO controller (Rapid GPIO)
 *
 * Functional model of one GPIO instance: output data (PDOR) with set/clear/
 * toggle aliases (PSOR/PCOR/PTOR), direction (PDDR) and input data (PDIR).
 * 32 input + 32 output qemu_irq lines make it a connectable controller.
 * Register offsets are from the MCXN947 CMSIS header (GPIO_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_GPIO_MCXN_GPIO_H
#define HW_GPIO_MCXN_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_GPIO "mcxn-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNGPIOState, MCXN_GPIO)

#define MCXN_GPIO_SIZE  0x1000
#define MCXN_GPIO_PINS  32

struct MCXNGPIOState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t pdor;      /* Port Data Output      */
    uint32_t pddr;      /* Port Data Direction (1 = output) */
    uint32_t pidr;      /* Port Input Disable    */
    uint32_t lock;      /* Lock                  */
    uint32_t in_level;  /* external input levels driven on the input lines */

    qemu_irq output[MCXN_GPIO_PINS];  /* one line per pin (output value) */
};

#endif /* HW_GPIO_MCXN_GPIO_H */
