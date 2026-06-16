/*
 * NXP MCX N MAILBOX (Inter-CPU Mailbox) — register-accurate model.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (MAILBOX_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_MAILBOX_H
#define HW_MISC_MCXN_MAILBOX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_MAILBOX "mcxn-mailbox"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNMailboxState, MCXN_MAILBOX)

#define MCXN_MAILBOX_SIZE 0x1000

struct MCXNMailboxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    /* Per-CPU pending interrupt request words (MBOXIRQ[n].IRQ). */
    uint32_t irq[2];
    /* MUTEX[EX] resource-availability bit. */
    uint32_t mutex;
};

#endif /* HW_MISC_MCXN_MAILBOX_H */
