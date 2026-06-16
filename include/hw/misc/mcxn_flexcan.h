/*
 * NXP MCX N FlexCAN (Flexible Controller Area Network, CAN FD) — bring-up model.
 *
 * Models the FlexCAN control registers accurately enough that firmware's
 * module-disable / freeze / soft-reset init handshakes settle and the init loop
 * terminates.  The full mapped window (control registers, message-buffer RAM,
 * individual mask RAM, enhanced RX FIFO filter RAM) is backed by a flat regs[]
 * array.  One QOM type serves both CAN0 and CAN1.  Offsets/bits from the
 * MCXN947 CMSIS header (CAN_Type); semantics from the FlexCAN chapter of the RM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FLEXCAN_H
#define HW_MISC_MCXN_FLEXCAN_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_FLEXCAN "mcxn-flexcan"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNFlexCanState, MCXN_FLEXCAN)

/*
 * CAN_Type spans control registers through the enhanced RX FIFO filter array
 * (ERFFEL[32] at offset 0x3000, ending at 0x3080).  CAN0 and CAN1 are spaced
 * 0x4000 apart in the memory map, so the mapped window is 0x4000.
 */
#define MCXN_FLEXCAN_SIZE 0x4000

struct MCXNFlexCanState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_FLEXCAN_SIZE / 4];
};

#endif /* HW_MISC_MCXN_FLEXCAN_H */
