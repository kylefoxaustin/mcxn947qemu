/*
 * NXP MCX N USBPHY — USB 2.0 high-speed integrated PHY register model.
 *
 * Register-accurate; no analog PHY behaviour.  The block lays its primary
 * registers out as quads: a base register followed by SET (+4), CLR (+8) and
 * TOG (+C) aliases that respectively OR, AND-NOT and XOR the base.  This model
 * resolves those aliases to the single backing word.  Firmware brings the PHY
 * out of reset by writing CTRL.SFTRST then clearing CTRL.CLKGATE; both bits
 * self-clear here so the PHY immediately reads ungated and out of reset.
 * VERSION is a read-only capability constant.  Offsets/bits from the MCXN947
 * CMSIS header (USBPHY_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_usbphy.h"
#include "migration/vmstate.h"

/* Base register offsets (CMSIS USBPHY_Type). */
#define R_CTRL      0x30    /* General Purpose Control (with SET/CLR/TOG) */
#define R_STATUS    0x40    /* RO-ish status */
#define R_VERSION   0x80    /* RO */

#define CTRL_CLKGATE    (1u << 30)  /* USBPHY_CTRL_CLKGATE */
#define CTRL_SFTRST     (1u << 31)  /* USBPHY_CTRL_SFTRST  */

/* VERSION: major 2, minor 0, step 0 — typical for this Sigmatel/NXP PHY IP. */
#define VERSION_VALUE   0x02000000u

/* Registers that carry SET/CLR/TOG aliases (the +4/+8/+C words after base). */
static bool usbphy_has_strobe(hwaddr base)
{
    switch (base) {
    case 0x00:  /* PWD       */
    case 0x10:  /* TX        */
    case 0x20:  /* RX        */
    case 0x30:  /* CTRL      */
    case 0x50:  /* DEBUG0    */
    case 0x90:  /* IP        */
    case 0xA0:  /* PLL_SIC   */
    case 0xC0:  /* USB1_VBUS_DETECT */
    case 0xD0:  /* USB1_VBUS_DET_STAT */
    case 0xE0:  /* USB1_CHRG_DETECT */
    case 0xF0:  /* USB1_CHRG_DET_STAT */
    case 0x100: /* ANACTRL   */
    case 0x130: /* TRIM_OVERRIDE_EN */
    case 0x140: /* PFDA      */
        return true;
    default:
        return false;
    }
}

static uint64_t mcxn_usbphy_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSBPHYState *s = MCXN_USBPHY(opaque);

    if (off >= MCXN_USBPHY_SIZE) {
        return 0;
    }

    switch (off) {
    case R_VERSION:
        return VERSION_VALUE;
    case R_CTRL:
        /* CLKGATE and SFTRST self-clear: PHY reads ungated and not in reset. */
        return s->regs[R_CTRL / 4] & ~(CTRL_CLKGATE | CTRL_SFTRST);
    default:
        return s->regs[off >> 2];
    }
}

static void mcxn_usbphy_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size)
{
    MCXNUSBPHYState *s = MCXN_USBPHY(opaque);
    uint32_t v = value;
    hwaddr base = off & ~0xFu;
    unsigned strobe = off & 0xFu;   /* 0=set-direct, 4=SET, 8=CLR, C=TOG */

    if (off >= MCXN_USBPHY_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    if (off == R_VERSION) {
        return;                         /* read-only */
    }

    if (usbphy_has_strobe(base) && strobe != 0) {
        uint32_t cur = s->regs[base >> 2];

        switch (strobe) {
        case 0x4: cur |= v;  break;     /* SET */
        case 0x8: cur &= ~v; break;     /* CLR */
        case 0xC: cur ^= v;  break;     /* TOG */
        default:  break;
        }
        s->regs[base >> 2] = cur;
        return;
    }

    s->regs[off >> 2] = v;
}

static const MemoryRegionOps mcxn_usbphy_ops = {
    .read = mcxn_usbphy_read,
    .write = mcxn_usbphy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_usbphy_reset(DeviceState *dev)
{
    MCXNUSBPHYState *s = MCXN_USBPHY(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_usbphy_realize(DeviceState *dev, Error **errp)
{
    MCXNUSBPHYState *s = MCXN_USBPHY(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_usbphy_ops, s,
                          TYPE_MCXN_USBPHY, MCXN_USBPHY_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_usbphy = {
    .name = TYPE_MCXN_USBPHY,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSBPHYState, MCXN_USBPHY_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_usbphy_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_usbphy_realize;
    device_class_set_legacy_reset(dc, mcxn_usbphy_reset);
    dc->vmsd = &vmstate_mcxn_usbphy;
}

static const TypeInfo mcxn_usbphy_types[] = {
    {
        .name          = TYPE_MCXN_USBPHY,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUSBPHYState),
        .class_init    = mcxn_usbphy_class_init,
    },
};

DEFINE_TYPES(mcxn_usbphy_types)
