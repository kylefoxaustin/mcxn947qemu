/*
 * NXP MCX N EWM (External Watchdog Monitor) - faithful register model.  See
 * the header.  Byte-wide registers; no real timeout is modelled, so configuring
 * or servicing the EWM never asserts the ewm_out_b signal.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_ewm.h"
#include "migration/vmstate.h"

#define R_CTRL         0x0   /* EWMEN/ASSIN/INEN/INTEN */
#define R_SERV         0x1   /* write-only service (0xB4 then 0x2C) */
#define R_CMPL         0x2   /* compare low  */
#define R_CMPH         0x3   /* compare high (reset 0xFF) */
#define R_CLKCTRL      0x4
#define R_CLKPRESCALER 0x5

static uint64_t mcxn_ewm_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNEWMState *s = MCXN_EWM(opaque);

    switch (off) {
    case R_CTRL:
        return s->ctrl;
    case R_SERV:
        return 0;            /* service register reads 0 */
    case R_CMPL:
        return s->cmpl;
    case R_CMPH:
        return s->cmph;
    case R_CLKCTRL:
        return s->clkctrl;
    case R_CLKPRESCALER:
        return s->clkprescaler;
    default:
        return 0;
    }
}

static void mcxn_ewm_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    MCXNEWMState *s = MCXN_EWM(opaque);
    uint8_t v = val;

    switch (off) {
    case R_CTRL:
        s->ctrl = v;
        break;
    case R_SERV:
        break;               /* service: no timeout modelled */
    case R_CMPL:
        s->cmpl = v;
        break;
    case R_CMPH:
        s->cmph = v;
        break;
    case R_CLKCTRL:
        s->clkctrl = v;
        break;
    case R_CLKPRESCALER:
        s->clkprescaler = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps mcxn_ewm_ops = {
    .read = mcxn_ewm_read,
    .write = mcxn_ewm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void mcxn_ewm_reset(DeviceState *dev)
{
    MCXNEWMState *s = MCXN_EWM(dev);

    s->ctrl = s->cmpl = s->clkctrl = s->clkprescaler = 0;
    s->cmph = 0xFF;                            /* RM reset value */
}

static void mcxn_ewm_realize(DeviceState *dev, Error **errp)
{
    MCXNEWMState *s = MCXN_EWM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_ewm_ops, s,
                          TYPE_MCXN_EWM, MCXN_EWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_ewm = {
    .name = TYPE_MCXN_EWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(ctrl, MCXNEWMState),
        VMSTATE_UINT8(cmpl, MCXNEWMState),
        VMSTATE_UINT8(cmph, MCXNEWMState),
        VMSTATE_UINT8(clkctrl, MCXNEWMState),
        VMSTATE_UINT8(clkprescaler, MCXNEWMState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_ewm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_ewm_realize;
    device_class_set_legacy_reset(dc, mcxn_ewm_reset);
    dc->vmsd = &vmstate_mcxn_ewm;
}

static const TypeInfo mcxn_ewm_types[] = {
    {
        .name          = TYPE_MCXN_EWM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEWMState),
        .class_init    = mcxn_ewm_class_init,
    },
};

DEFINE_TYPES(mcxn_ewm_types)
