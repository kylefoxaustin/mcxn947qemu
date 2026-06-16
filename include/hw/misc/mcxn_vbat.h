/*
 * NXP MCX N VBAT (VBAT / always-on domain) — bring-up model.
 *
 * Models the always-on (RTC backup) domain register file. The oscillator and
 * LDO "ready" status bits in STATUSA read ready so firmware polling completes.
 * STATUSA/STATUSB flag bits are write-1-to-clear. Offsets/bits from the
 * MCXN947 CMSIS header (VBAT_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_VBAT_H
#define HW_MISC_MCXN_VBAT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_VBAT "mcxn-vbat"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNVBATState, MCXN_VBAT)

#define MCXN_VBAT_SIZE 0x1000

struct MCXNVBATState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_VBAT_SIZE / 4];
};

#endif /* HW_MISC_MCXN_VBAT_H */
