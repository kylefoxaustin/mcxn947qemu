/*
 * NXP MCX N USBHS1 sub-blocks — register models.
 *
 * Register-accurate; no actual USB transfers.  Three SysBusDevices map the
 * three windows of the high-speed USB1 instance:
 *
 *   mcxn-usbhs-phydcd (0x800)  USBHS1 PHY/DCD region (CMSIS USBHSDCD_Type).
 *                              Modelled as a permissive readback array over the
 *                              whole window; CONTROL.SR/START self-clear like
 *                              the standalone USBDCD so firmware sequencing
 *                              completes.
 *   mcxn-usbhs-core   (0x200)  EHCI-style controller (CMSIS USBHS_Type).
 *                              USBCMD.RST (bit 1) self-clears; USBSTS.HCH
 *                              (bit 12, HCHalted) tracks USBCMD.RS (run/stop)
 *                              so a halted controller reads halted; USBSTS is
 *                              W1C for the interrupt-status bits; the ID and
 *                              EHCI capability registers are read-only.
 *   mcxn-usbhs-nc     (0xE00)  Non-core control (CMSIS USBNC_Type).  Permissive
 *                              readback array over the whole window.
 *
 * Offsets/bits from the MCXN947 CMSIS header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_usbhs.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* ---- USBHS1 PHY/DCD (USBHSDCD_Type, identical layout to USBDCD_Type) ---- */

#define DCD_CONTROL        0x00
#define DCD_STATUS         0x08    /* RO */

#define DCD_CONTROL_IACK   (1u << 0)
#define DCD_CONTROL_START  (1u << 24)
#define DCD_CONTROL_SR     (1u << 25)

static uint64_t usbhs_phydcd_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBHSPhyDcdState *s = MCXN_USBHS_PHYDCD(opaque);

    if (off >= MCXN_USBHS_PHYDCD_SIZE) {
        return 0;
    }
    if (off == DCD_CONTROL) {
        return s->regs[DCD_CONTROL / 4] &
               ~(DCD_CONTROL_IACK | DCD_CONTROL_START | DCD_CONTROL_SR);
    }
    return s->regs[off >> 2];
}

static void usbhs_phydcd_write(void *opaque, hwaddr off, uint64_t value,
                               unsigned size)
{
    MCXNUSBHSPhyDcdState *s = MCXN_USBHS_PHYDCD(opaque);
    uint32_t v = value;

    if (off >= MCXN_USBHS_PHYDCD_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (off == DCD_STATUS) {
        return;                         /* read-only */
    }
    if (off == DCD_CONTROL) {
        s->regs[DCD_CONTROL / 4] =
            v & ~(DCD_CONTROL_IACK | DCD_CONTROL_START | DCD_CONTROL_SR);
        return;
    }
    s->regs[off >> 2] = v;
}

static const MemoryRegionOps usbhs_phydcd_ops = {
    .read = usbhs_phydcd_read,
    .write = usbhs_phydcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void usbhs_phydcd_reset(DeviceState *dev)
{
    MCXNUSBHSPhyDcdState *s = MCXN_USBHS_PHYDCD(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void usbhs_phydcd_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBHSPhyDcdState *s = MCXN_USBHS_PHYDCD(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &usbhs_phydcd_ops, s,
                          TYPE_MCXN_USBHS_PHYDCD, MCXN_USBHS_PHYDCD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_usbhs_phydcd = {
    .name = TYPE_MCXN_USBHS_PHYDCD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBHSPhyDcdState,
                             MCXN_USBHS_PHYDCD_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void usbhs_phydcd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = usbhs_phydcd_realize;
    device_class_set_legacy_reset(dc, usbhs_phydcd_reset);
    dc->vmsd = &vmstate_usbhs_phydcd;
}

/* ---- USBHS1 core: EHCI-style controller (USBHS_Type) ---- */

#define HS_ID           0x000   /* RO */
#define HS_HWGENERAL    0x004   /* RO */
#define HS_HWHOST       0x008   /* RO */
#define HS_HWDEVICE     0x00C   /* RO */
#define HS_HWTXBUF      0x010   /* RO */
#define HS_HWRXBUF      0x014   /* RO */
#define HS_CAPLENGTH    0x100   /* RO (CAPLENGTH + HCIVERSION packed) */
#define HS_HCSPARAMS    0x104   /* RO */
#define HS_HCCPARAMS    0x108   /* RO */
#define HS_DCIVERSION   0x120   /* RO */
#define HS_DCCPARAMS    0x124   /* RO */
#define HS_USBCMD       0x140
#define HS_USBSTS       0x144   /* W1C status bits */
#define HS_USBINTR      0x148
#define HS_USBMODE      0x1A8

#define USBCMD_RS       (1u << 0)   /* Run/Stop */
#define USBCMD_RST      (1u << 1)   /* Controller reset, self-clearing */
#define USBSTS_HCH      (1u << 12)  /* HCHalted */

/*
 * EHCI capability constants for this controller.  CAPLENGTH = 0x40 (operational
 * registers begin 0x40 past CAPLENGTH, i.e. at 0x140) with HCIVERSION 0x0100 in
 * the upper half-word.  Values are the documented NXP/Chipidea defaults; mark
 * as approximate where the RM register-diagram reset was not machine-readable.
 */
#define HS_ID_VALUE          0x0022FA05u   /* ID/NID/REVISION, RM-approximate */
#define HS_CAPLENGTH_VALUE   0x01000040u   /* HCIVERSION:0x0100, CAPLENGTH:0x40 */
#define HS_HCSPARAMS_VALUE   0x00010011u   /* 1 port, host capable, approximate */
#define HS_HCCPARAMS_VALUE   0x00000006u   /* async sched park + prog frame list */
#define HS_DCIVERSION_VALUE  0x00000001u
#define HS_DCCPARAMS_VALUE   0x00000188u   /* device capable, host capable, 8 EP */

/* USBSTS interrupt-status bits are W1C; HCH is status-only (not W1C). */
#define USBSTS_W1C_MASK      0x000003FFu

static uint64_t usbhs_core_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBHSCoreState *s = MCXN_USBHS_CORE(opaque);
    uint32_t v;

    if (off >= MCXN_USBHS_CORE_SIZE) {
        return 0;
    }

    switch (off) {
    case HS_ID:         return HS_ID_VALUE;
    case HS_HWGENERAL:  return 0;
    case HS_HWHOST:     return 0x10020001u;   /* host present, approximate */
    case HS_HWDEVICE:   return 0x00000005u;   /* device present, approximate */
    case HS_HWTXBUF:    return 0x80040020u;   /* approximate */
    case HS_HWRXBUF:    return 0x00000020u;   /* approximate */
    case HS_CAPLENGTH:  return HS_CAPLENGTH_VALUE;
    case HS_HCSPARAMS:  return HS_HCSPARAMS_VALUE;
    case HS_HCCPARAMS:  return HS_HCCPARAMS_VALUE;
    case HS_DCIVERSION: return HS_DCIVERSION_VALUE;
    case HS_DCCPARAMS:  return HS_DCCPARAMS_VALUE;
    case HS_USBCMD:
        /* RST is self-clearing. */
        return s->regs[HS_USBCMD / 4] & ~USBCMD_RST;
    case HS_USBSTS:
        /* HCHalted reflects run/stop: halted whenever RS is clear. */
        v = s->regs[HS_USBSTS / 4] & ~USBSTS_HCH;
        if (!(s->regs[HS_USBCMD / 4] & USBCMD_RS)) {
            v |= USBSTS_HCH;
        }
        return v;
    default:
        return s->regs[off >> 2];
    }
}

static void usbhs_core_update_irq(MCXNUSBHSCoreState *s)
{
    bool active = (s->regs[HS_USBSTS / 4] & s->regs[HS_USBINTR / 4] &
                   USBSTS_W1C_MASK) != 0;

    qemu_set_irq(s->irq, active);
}

static void usbhs_core_write(void *opaque, hwaddr off, uint64_t value,
                             unsigned size)
{
    MCXNUSBHSCoreState *s = MCXN_USBHS_CORE(opaque);
    uint32_t v = value;

    if (off >= MCXN_USBHS_CORE_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case HS_ID:
    case HS_HWGENERAL:
    case HS_HWHOST:
    case HS_HWDEVICE:
    case HS_HWTXBUF:
    case HS_HWRXBUF:
    case HS_CAPLENGTH:
    case HS_HCSPARAMS:
    case HS_HCCPARAMS:
    case HS_DCIVERSION:
    case HS_DCCPARAMS:
        return;                         /* read-only capability registers */
    case HS_USBCMD:
        /* RST self-clears; store the rest (RS etc.). */
        s->regs[HS_USBCMD / 4] = v & ~USBCMD_RST;
        if (v & USBCMD_RST) {
            /* Reset returns operational registers to defaults. */
            s->regs[HS_USBCMD / 4] = 0;
            s->regs[HS_USBSTS / 4] = 0;
            s->regs[HS_USBINTR / 4] = 0;
        }
        usbhs_core_update_irq(s);
        return;
    case HS_USBSTS:
        s->regs[HS_USBSTS / 4] &= ~(v & USBSTS_W1C_MASK);   /* W1C */
        usbhs_core_update_irq(s);
        return;
    case HS_USBINTR:
        s->regs[HS_USBINTR / 4] = v;
        usbhs_core_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps usbhs_core_ops = {
    .read = usbhs_core_read,
    .write = usbhs_core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void usbhs_core_reset(DeviceState *dev)
{
    MCXNUSBHSCoreState *s = MCXN_USBHS_CORE(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void usbhs_core_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBHSCoreState *s = MCXN_USBHS_CORE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &usbhs_core_ops, s,
                          TYPE_MCXN_USBHS_CORE, MCXN_USBHS_CORE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_usbhs_core = {
    .name = TYPE_MCXN_USBHS_CORE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBHSCoreState,
                             MCXN_USBHS_CORE_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void usbhs_core_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = usbhs_core_realize;
    device_class_set_legacy_reset(dc, usbhs_core_reset);
    dc->vmsd = &vmstate_usbhs_core;
}

/* ---- USBHS1 non-core control (USBNC_Type) ---- */

static uint64_t usbhs_nc_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBHSNcState *s = MCXN_USBHS_NC(opaque);

    return (off < MCXN_USBHS_NC_SIZE) ? s->regs[off >> 2] : 0;
}

static void usbhs_nc_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    MCXNUSBHSNcState *s = MCXN_USBHS_NC(opaque);

    if (off >= MCXN_USBHS_NC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps usbhs_nc_ops = {
    .read = usbhs_nc_read,
    .write = usbhs_nc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void usbhs_nc_reset(DeviceState *dev)
{
    MCXNUSBHSNcState *s = MCXN_USBHS_NC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void usbhs_nc_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBHSNcState *s = MCXN_USBHS_NC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &usbhs_nc_ops, s,
                          TYPE_MCXN_USBHS_NC, MCXN_USBHS_NC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_usbhs_nc = {
    .name = TYPE_MCXN_USBHS_NC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBHSNcState, MCXN_USBHS_NC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void usbhs_nc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = usbhs_nc_realize;
    device_class_set_legacy_reset(dc, usbhs_nc_reset);
    dc->vmsd = &vmstate_usbhs_nc;
}

/* ---- Type registration ---- */

static const TypeInfo mcxn_usbhs_types[] = {
    {
        .name          = TYPE_MCXN_USBHS_PHYDCD,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUSBHSPhyDcdState),
        .class_init    = usbhs_phydcd_class_init,
    },
    {
        .name          = TYPE_MCXN_USBHS_CORE,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUSBHSCoreState),
        .class_init    = usbhs_core_class_init,
    },
    {
        .name          = TYPE_MCXN_USBHS_NC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUSBHSNcState),
        .class_init    = usbhs_nc_class_init,
    },
};

DEFINE_TYPES(mcxn_usbhs_types)
