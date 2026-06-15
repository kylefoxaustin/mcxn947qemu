/*
 * NXP MCX N SPC (System Power Controller) — bring-up model.
 *
 * Firmware programs SRAM/core/system voltage and polls for completion.  The key
 * blocker for bring-up is SRAMCTL's REQ->ACK handshake: software sets REQ
 * (bit 30) and spins until hardware sets ACK (bit 31).  Here ACK simply tracks
 * REQ (instant completion).  SC.BUSY reads idle.  All other registers are
 * permissively backed.  Offsets/bits from the MCXN947 CMSIS header (SPC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_spc.h"
#include "migration/vmstate.h"

#define SPC_VERID    0x00   /* RO */
#define SPC_SC       0x10   /* Status Control */
#define SPC_SRAMCTL  0x40   /* SRAM Control */

#define SPC_SC_BUSY        (1u << 0)
#define SPC_SRAMCTL_REQ    (1u << 30)
#define SPC_SRAMCTL_ACK    (1u << 31)

#define SPC_VERID_VALUE    0x00000001u

static uint64_t mcxn_spc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSPCState *s = MCXN_SPC(opaque);
    uint32_t val;

    switch (offset) {
    case SPC_VERID:
        return SPC_VERID_VALUE;
    case SPC_SC:
        /* Power transitions are instantaneous in the model: never busy. */
        return s->regs[SPC_SC / 4] & ~SPC_SC_BUSY;
    case SPC_SRAMCTL:
        /* Acknowledge instantly: ACK mirrors REQ. */
        val = s->regs[SPC_SRAMCTL / 4];
        if (val & SPC_SRAMCTL_REQ) {
            val |= SPC_SRAMCTL_ACK;
        } else {
            val &= ~SPC_SRAMCTL_ACK;
        }
        return val;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_spc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNSPCState *s = MCXN_SPC(opaque);

    if (offset == SPC_VERID) {
        return;  /* read-only */
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_spc_ops = {
    .read = mcxn_spc_read,
    .write = mcxn_spc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_spc_reset(DeviceState *dev)
{
    MCXNSPCState *s = MCXN_SPC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_spc_realize(DeviceState *dev, Error **errp)
{
    MCXNSPCState *s = MCXN_SPC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_spc_ops, s,
                          TYPE_MCXN_SPC, MCXN_SPC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_spc = {
    .name = TYPE_MCXN_SPC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSPCState, MCXN_SPC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_spc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_spc_realize;
    device_class_set_legacy_reset(dc, mcxn_spc_reset);
    dc->vmsd = &vmstate_mcxn_spc;
}

static const TypeInfo mcxn_spc_types[] = {
    {
        .name          = TYPE_MCXN_SPC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSPCState),
        .class_init    = mcxn_spc_class_init,
    },
};

DEFINE_TYPES(mcxn_spc_types)
