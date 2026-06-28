/*
 * NXP MCX N FlexSPI (Flexible Serial Peripheral Interface) — bring-up model.
 *
 * Models just enough of the FlexSPI controller for firmware external-flash
 * init to complete without a real flash backend:
 *
 *   - MCR0.SWRESET is momentary in hardware; here it self-clears so the
 *     "set SWRESET, wait for it to clear" reset handshake terminates.
 *   - STS0 always reports the controller idle (ARBIDLE=1, SEQIDLE=1) so the
 *     "wait until idle" guard before issuing a command passes immediately.
 *   - Launching an IP command via IPCMD.TRG raises INTR.IPCMDDONE.  The status
 *     bits in INTR are write-1-to-clear, so the canonical "trigger IP command,
 *     poll IPCMDDONE, clear it" loop resolves.
 *   - IP RX/TX FIFO status (IPRXFSTS/IPTXFSTS) reads as empty so FIFO fill/drain
 *     polling does not spin.
 *
 * No flash data is produced; IP RX FIFO data reads back zero.  Offsets and bit
 * masks come from the MCXN947 CMSIS header (FLEXSPI_Type).  This FlexSPI map
 * has no VERID register.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/misc/mcxn_flexspi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS FLEXSPI_Type). */
#define FLEXSPI_MCR0        0x00
#define FLEXSPI_MCR1        0x04
#define FLEXSPI_MCR2        0x08
#define FLEXSPI_AHBCR       0x0C
#define FLEXSPI_INTEN       0x10
#define FLEXSPI_INTR        0x14    /* W1C status */
#define FLEXSPI_LUTKEY      0x18
#define FLEXSPI_LUTCR       0x1C
#define FLEXSPI_IPCR0       0xA0
#define FLEXSPI_IPCR1       0xA4
#define FLEXSPI_IPCR2       0xA8
#define FLEXSPI_IPCMD       0xB0    /* WO trigger */
#define FLEXSPI_DLPR        0xB4
#define FLEXSPI_IPRXFCR     0xB8
#define FLEXSPI_IPTXFCR     0xBC
#define FLEXSPI_STS0        0xE0    /* RO */
#define FLEXSPI_STS1        0xE4    /* RO */
#define FLEXSPI_STS2        0xE8    /* RO */
#define FLEXSPI_AHBSPNDSTS  0xEC    /* RO */
#define FLEXSPI_IPRXFSTS    0xF0    /* RO */
#define FLEXSPI_IPTXFSTS    0xF4    /* RO */
#define FLEXSPI_RFDR0       0x100   /* RFDR[32] @0x100..0x17C, RO */
#define FLEXSPI_TFDR0       0x180   /* TFDR[32] @0x180..0x1FC, WO */

/* MCR0 bits. */
#define MCR0_SWRESET   (1u << 0)

/* INTR bits. */
#define INTR_IPCMDDONE (1u << 0)

/* IPCMD bits. */
#define IPCMD_TRG      (1u << 0)

/* STS0 bits. */
#define STS0_SEQIDLE   (1u << 0)
#define STS0_ARBIDLE   (1u << 1)

/* STS0 idle report: both arbiter and sequencer idle. */
#define STS0_IDLE      (STS0_SEQIDLE | STS0_ARBIDLE)

static void mcxn_flexspi_update_irq(MCXNFlexSPIState *s)
{
    uint32_t active = s->regs[FLEXSPI_INTR >> 2] &
                      s->regs[FLEXSPI_INTEN >> 2];
    qemu_set_irq(s->irq, active != 0);
}

static uint64_t mcxn_flexspi_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(opaque);
    uint32_t v = (off < MCXN_FLEXSPI_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case FLEXSPI_MCR0:
        /* SWRESET is momentary: never read back as set. */
        return v & ~MCR0_SWRESET;
    case FLEXSPI_STS0:
        /* Controller always idle so the pre-command idle wait passes. */
        return STS0_IDLE;
    case FLEXSPI_STS1:
    case FLEXSPI_STS2:
    case FLEXSPI_AHBSPNDSTS:
        return 0;
    case FLEXSPI_IPRXFSTS:
    case FLEXSPI_IPTXFSTS:
        /* FIFO fill level reads as empty. */
        return 0;
    default:
        if (off >= FLEXSPI_RFDR0 && off < FLEXSPI_RFDR0 + 32 * 4) {
            return 0;   /* IP RX FIFO data: no flash data modelled */
        }
        return v;
    }
}

static void mcxn_flexspi_write(void *opaque, hwaddr off, uint64_t value,
                               unsigned size)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(opaque);
    uint32_t val = value;

    if (off >= MCXN_FLEXSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case FLEXSPI_STS0:
    case FLEXSPI_STS1:
    case FLEXSPI_STS2:
    case FLEXSPI_AHBSPNDSTS:
    case FLEXSPI_IPRXFSTS:
    case FLEXSPI_IPTXFSTS:
        return;   /* read-only status */
    case FLEXSPI_MCR0:
        /* Latch but drop the self-clearing SWRESET bit. */
        s->regs[off >> 2] = val & ~MCR0_SWRESET;
        return;
    case FLEXSPI_INTR:
        /* Write-1-to-clear status bits. */
        s->regs[off >> 2] &= ~val;
        mcxn_flexspi_update_irq(s);
        return;
    case FLEXSPI_INTEN:
        s->regs[off >> 2] = val;
        mcxn_flexspi_update_irq(s);
        return;
    case FLEXSPI_IPCMD:
        /* Launching an IP command completes it instantly. */
        if (val & IPCMD_TRG) {
            s->regs[FLEXSPI_INTR >> 2] |= INTR_IPCMDDONE;
            mcxn_flexspi_update_irq(s);
        }
        return;
    default:
        if (off >= FLEXSPI_RFDR0 && off < FLEXSPI_RFDR0 + 32 * 4) {
            return;   /* IP RX FIFO data is read-only */
        }
        s->regs[off >> 2] = val;
        return;
    }
}

static const MemoryRegionOps mcxn_flexspi_ops = {
    .read = mcxn_flexspi_read,
    .write = mcxn_flexspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_flexspi_reset(DeviceState *dev)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_flexspi_realize(DeviceState *dev, Error **errp)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(dev);

    /* MMIO region 0: the CMSIS register file. */
    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_flexspi_ops, s,
                          TYPE_MCXN_FLEXSPI, MCXN_FLEXSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /* MMIO region 1: the AHB-mapped external NOR.  RAM-backed so the SoC's
     * AHB window holds real, executable flash contents — the -kernel loader
     * fills it and code linked there runs in place (XIP). */
    memory_region_init_ram(&s->nor, OBJECT(s), "mcxn.flexspi-nor",
                           s->flash_size, &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->nor);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property mcxn_flexspi_props[] = {
    /* Size of the AHB-mapped NOR window.  Default = the FRDM-MCXN947's
     * 8 MiB Winbond W25Q64 (Zephyr DTS: ranges @ 0x9000_0000, DT_SIZE_M(8)). */
    DEFINE_PROP_UINT64("flash-size", MCXNFlexSPIState, flash_size, 8 * MiB),
};

static const VMStateDescription vmstate_mcxn_flexspi = {
    .name = TYPE_MCXN_FLEXSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNFlexSPIState, MCXN_FLEXSPI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_flexspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_flexspi_realize;
    device_class_set_legacy_reset(dc, mcxn_flexspi_reset);
    device_class_set_props(dc, mcxn_flexspi_props);
    dc->vmsd = &vmstate_mcxn_flexspi;
}

static const TypeInfo mcxn_flexspi_types[] = {
    {
        .name          = TYPE_MCXN_FLEXSPI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNFlexSPIState),
        .class_init    = mcxn_flexspi_class_init,
    },
};

DEFINE_TYPES(mcxn_flexspi_types)
