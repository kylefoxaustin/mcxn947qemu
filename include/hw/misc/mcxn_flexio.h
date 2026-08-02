/*
 * NXP MCX N FlexIO (Flexible I/O) — bring-up model.
 *
 * Presents the FlexIO register window with the RM reset values: VERID/PARAM are
 * read-only identification constants and the shifter/timer/pin status registers
 * read back idle so firmware bring-up completes.  Offsets from the MCXN947 CMSIS
 * header (FLEXIO_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FLEXIO_H
#define HW_MISC_MCXN_FLEXIO_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_MCXN_FLEXIO "mcxn-flexio"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNFlexIOState, MCXN_FLEXIO)

#define MCXN_FLEXIO_SIZE 0x1000

struct MCXNFlexIOState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    /* FlexIO-as-SPI-master: a real SSI bus (the SoC attaches an m25p80 NOR) and a
     * chip-select gpio driven by a FlexIO output pin. */
    SSIBus  *spi_bus;
    qemu_irq spi_cs;
    uint32_t regs[MCXN_FLEXIO_SIZE / 4];
};

#endif /* HW_MISC_MCXN_FLEXIO_H */
