/*
 * NXP MCX N CMC (Core Mode Controller) — register-accurate model.
 *
 * The CMC handles core/system power-mode requests and reports the reset reason.
 * In emulation it has no externally-observable behavior beyond register
 * read-back: a mode request stored here must NOT actually reset the machine,
 * and the reset-reason status registers read a benign "normal power-on" value.
 *
 *   - VERID is read-only and returns a constant version ID.
 *   - GPMCTRL is write-only (CMSIS __O); reads return 0.
 *   - SRS / RSTCNT are read-only status (CMSIS __I).
 *   - SRS reports POR + VD set, i.e. a normal power-on reset reason.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (CMC_Type); reset
 * values from the MCX N Reference Manual (section 38.7.1).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_cmc.h"
#include "migration/vmstate.h"

#define CMC_VERID     0x00   /* RO  Version ID */
#define CMC_CKCTRL    0x10   /* RW  Clock Control */
#define CMC_CKSTAT    0x14   /* RW  Clock Status */
#define CMC_PMPROT    0x18   /* RW  Power Mode Protection */
#define CMC_GPMCTRL   0x1C   /* WO  Global Power Mode Control */
#define CMC_PMCTRL0   0x20   /* RW  Power Mode Control (MAIN) */
#define CMC_PMCTRL1   0x24   /* RW  Power Mode Control (WAKE) */
#define CMC_SRS       0x80   /* RO  System Reset Status */
#define CMC_RPC       0x84   /* RW  Reset Pin Control */
#define CMC_SSRS      0x88   /* RW  Sticky System Reset Status */
#define CMC_SRIE      0x8C   /* RW  System Reset Interrupt Enable */
#define CMC_SRIF      0x90   /* RW  System Reset Interrupt Flag */
#define CMC_RSTCNT    0x9C   /* RO  Reset Count Register */
#define CMC_MR0       0xA0   /* RW  Mode */
#define CMC_FM0       0xB0   /* RW  Force Mode */
#define CMC_SRAMDIS0  0xC0   /* RW  SRAM Disable */
#define CMC_SRAMRET0  0xD0   /* RW  SRAM Retention */
#define CMC_FLASHCR   0xE0   /* RW  Flash Control */
#define CMC_BSR       0x100  /* RW  BootROM Status Register */
#define CMC_BLR       0x10C  /* RW  BootROM Lock Register */
#define CMC_CORECTL   0x110  /* RW  Core Control */
#define CMC_DBGCTL    0x120  /* RW  Debug Control */

/* Constant version ID (RM reset value). */
#define CMC_VERID_VALUE   0x03010000u

/*
 * Reset reason: POR (bit 1) and VD (bit 2) are both set after a power-on
 * reset, i.e. a benign "normal power-on".  SSRS is the sticky mirror and shares
 * the same power-on value.
 */
#define CMC_SRS_POR       (1u << 1)
#define CMC_SRS_VD        (1u << 2)
#define CMC_SRS_POR_VALUE (CMC_SRS_POR | CMC_SRS_VD)   /* 0x6 */

/* Non-zero RM reset values for backed (RW) registers. */
#define CMC_SSRS_RESET    0x00000006u
#define CMC_SRIE_RESET    0x00008800u
#define CMC_BLR_RESET     0x00000002u

static uint64_t mcxn_cmc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNCMCState *s = MCXN_CMC(opaque);

    switch (offset) {
    case CMC_VERID:
        return CMC_VERID_VALUE;
    case CMC_GPMCTRL:
        /* Write-only register: reads return 0. */
        return 0;
    case CMC_SRS:
        /* Read-only reset-reason status: benign normal power-on. */
        return CMC_SRS_POR_VALUE;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_cmc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNCMCState *s = MCXN_CMC(opaque);

    switch (offset) {
    case CMC_VERID:
    case CMC_SRS:
    case CMC_RSTCNT:
        /* Read-only registers: ignore writes. */
        return;
    default:
        /*
         * Mode/force-mode and all other control registers are simply stored.
         * A power-mode request must NOT actually reset or power down the
         * machine, so there is no side effect here.
         */
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_cmc_ops = {
    .read = mcxn_cmc_read,
    .write = mcxn_cmc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_cmc_reset(DeviceState *dev)
{
    MCXNCMCState *s = MCXN_CMC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[CMC_SSRS / 4] = CMC_SSRS_RESET;
    s->regs[CMC_SRIE / 4] = CMC_SRIE_RESET;
    s->regs[CMC_BLR / 4] = CMC_BLR_RESET;
}

static void mcxn_cmc_realize(DeviceState *dev, Error **errp)
{
    MCXNCMCState *s = MCXN_CMC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_cmc_ops, s,
                          TYPE_MCXN_CMC, MCXN_CMC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_cmc = {
    .name = TYPE_MCXN_CMC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNCMCState, MCXN_CMC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_cmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_cmc_realize;
    device_class_set_legacy_reset(dc, mcxn_cmc_reset);
    dc->vmsd = &vmstate_mcxn_cmc;
}

static const TypeInfo mcxn_cmc_types[] = {
    {
        .name          = TYPE_MCXN_CMC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCMCState),
        .class_init    = mcxn_cmc_class_init,
    },
};

DEFINE_TYPES(mcxn_cmc_types)
