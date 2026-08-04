/*
 * NXP MCX N PORT (pin mux / pin control) — bring-up stub.
 *
 * Firmware programs the PORT PCR[] registers to mux pins and set pull/drive.
 * Pin muxing does not change the emulated GPIO/peripheral function, so this is
 * a permissive register-backed stub: writes are stored, reads return them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_PORT_H
#define HW_MISC_MCXN_PORT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_PORT "mcxn-port"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNPortState, MCXN_PORT)

#define MCXN_PORT_SIZE 0x1000

struct MCXNPortState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_PORT_SIZE / 4];
    /* which PORT this is -- the RM's pad resets DIFFER per port */
    uint8_t  port_id;
};

#endif /* HW_MISC_MCXN_PORT_H */
