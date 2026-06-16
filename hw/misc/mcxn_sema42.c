/*
 * NXP MCX N SEMA42 (Hardware Semaphores) — register-accurate model.
 *
 * SEMA42 provides 16 hardware "gate" semaphores used to arbitrate shared
 * resources between processing domains.  Each gate is a single byte holding a
 * 4-bit finite state machine (GTFSM): 0 means the gate is free, while value
 * (d + 1) means domain d currently owns the lock.
 *
 * Lock/unlock protocol modelled here:
 *   - Writing a non-zero domain value to a free gate locks it for that domain.
 *   - Writing the same domain value to a gate it already owns is a no-op.
 *   - Writing a domain value to a gate owned by a different domain is ignored
 *     (the lock attempt fails; the gate keeps its current owner).
 *   - Writing 0 unlocks the gate (release).
 * Reads return the current GTFSM owner state.
 *
 * RSTGT (offset 0x42, 16-bit) is the secure gate-reset register.  RSTGT_W is
 * write-only and RSTGT_R is read-only; the read side reports the reset-state
 * machine.  In this model the reset sequence is not decoded for side effects;
 * RSTGT reads back its stored value and resets to 0.
 *
 * The gate registers are byte-wide, so this device permits 1, 2, and 4-byte
 * accesses.  Offsets/bits/access-types from the MCXN947 CMSIS header
 * (SEMA42_Type); reset values from the MCX N Reference Manual (chapter 27,
 * all gates free / RSTGT 0).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_sema42.h"
#include "migration/vmstate.h"

#define SEMA42_GATE_LAST   0x0F
#define SEMA42_RSTGT       0x42   /* 16-bit: RSTGT_R (RO) / RSTGT_W (WO) */

#define SEMA42_GATE_GTFSM_MASK 0x0F

static uint64_t mcxn_sema42_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSema42State *s = MCXN_SEMA42(opaque);

    if (offset <= SEMA42_GATE_LAST) {
        return s->gate[offset] & SEMA42_GATE_GTFSM_MASK;
    }
    if (offset == SEMA42_RSTGT) {
        return s->rstgt;
    }
    return 0;
}

static void mcxn_sema42_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MCXNSema42State *s = MCXN_SEMA42(opaque);

    if (offset <= SEMA42_GATE_LAST) {
        uint8_t domain = value & SEMA42_GATE_GTFSM_MASK;
        uint8_t cur = s->gate[offset] & SEMA42_GATE_GTFSM_MASK;

        if (domain == 0) {
            /* Release: unlock the gate. */
            s->gate[offset] = 0;
        } else if (cur == 0 || cur == domain) {
            /* Lock a free gate, or re-affirm an owned gate. */
            s->gate[offset] = domain;
        }
        /* Otherwise the gate is owned by another domain: lock attempt fails. */
        return;
    }
    if (offset == SEMA42_RSTGT) {
        /* RSTGT_W is write-only; store low byte for benign read-back. */
        s->rstgt = value & 0xFF;
        return;
    }
}

static const MemoryRegionOps mcxn_sema42_ops = {
    .read = mcxn_sema42_read,
    .write = mcxn_sema42_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_sema42_reset(DeviceState *dev)
{
    MCXNSema42State *s = MCXN_SEMA42(dev);

    memset(s->gate, 0, sizeof(s->gate));
    s->rstgt = 0;
}

static void mcxn_sema42_realize(DeviceState *dev, Error **errp)
{
    MCXNSema42State *s = MCXN_SEMA42(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_sema42_ops, s,
                          TYPE_MCXN_SEMA42, MCXN_SEMA42_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_sema42 = {
    .name = TYPE_MCXN_SEMA42,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(gate, MCXNSema42State, MCXN_SEMA42_NUM_GATES),
        VMSTATE_UINT16(rstgt, MCXNSema42State),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_sema42_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_sema42_realize;
    device_class_set_legacy_reset(dc, mcxn_sema42_reset);
    dc->vmsd = &vmstate_mcxn_sema42;
}

static const TypeInfo mcxn_sema42_types[] = {
    {
        .name          = TYPE_MCXN_SEMA42,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSema42State),
        .class_init    = mcxn_sema42_class_init,
    },
};

DEFINE_TYPES(mcxn_sema42_types)
