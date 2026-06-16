/*
 * NXP MCX N FMU (Flash Management Unit) — functional flash controller.  See
 * header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_fmu.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define R_FSTAT   0x0
#define R_FCNFG   0x4
#define R_FCTRL   0x8
#define R_FCCOB0  0x10   /* FCCOB0..7 @0x10..0x2C */

#define FSTAT_FAIL   (1u << 0)
#define FSTAT_CMDABT (1u << 2)
#define FSTAT_ACCERR (1u << 5)
#define FSTAT_PEWERR (1u << 6)
#define FSTAT_CCIF   (1u << 7)   /* 1 = command complete / idle */
#define FCNFG_CCIE   (1u << 7)

/* FCMD command codes (RM Chapter 8 command summary). */
#define FCMD_RD1ALL  0x00
#define FCMD_RD1BLK  0x01
#define FCMD_RD1SCR  0x02
#define FCMD_RD1PHR  0x04
#define FCMD_PGMPG   0x23
#define FCMD_PGMPHR  0x24
#define FCMD_ERSALL  0x40
#define FCMD_ERSSCR  0x42

#define MCXN_FLASH_SECTOR 0x2000   /* 8 KB erase sector */

static uint8_t *fmu_flash_ptr(MCXNFMUState *s)
{
    return s->flash ? memory_region_get_ram_ptr(s->flash) : NULL;
}

/* Map a flash command address (any aperture) to an offset into the array. */
static uint64_t fmu_flash_off(MCXNFMUState *s, uint64_t addr)
{
    return s->flash_size ? (addr & (s->flash_size - 1)) : 0;
}

static void fmu_update_irq(MCXNFMUState *s)
{
    /* The interrupt asserts when the command completes and CCIE is set. */
    qemu_set_irq(s->irq, (s->fstat & FSTAT_CCIF) && (s->fcnfg & FCNFG_CCIE));
}

/* Execute the command currently programmed in FCCOB and re-assert CCIF. */
static void fmu_launch(MCXNFMUState *s)
{
    uint8_t cmd = s->fccob[0] & 0xFF;
    uint64_t addr = s->fccob[1];
    uint8_t *flash = fmu_flash_ptr(s);
    uint64_t off;

    s->fstat &= ~(FSTAT_FAIL | FSTAT_ACCERR | FSTAT_PEWERR | FSTAT_CMDABT);

    switch (cmd) {
    case FCMD_ERSALL:
        if (flash) {
            memset(flash, 0xFF, s->flash_size);
        }
        break;
    case FCMD_ERSSCR:
        if (flash) {
            off = fmu_flash_off(s, addr) & ~(uint64_t)(MCXN_FLASH_SECTOR - 1);
            if (off + MCXN_FLASH_SECTOR <= s->flash_size) {
                memset(flash + off, 0xFF, MCXN_FLASH_SECTOR);
            }
        }
        break;
    case FCMD_PGMPG:
    case FCMD_PGMPHR:
        /* Program data is written directly to the flash address by firmware
         * and lands in the RAM-backed flash; nothing to do here. */
        break;
    case FCMD_RD1ALL:
    case FCMD_RD1BLK:
    case FCMD_RD1SCR:
    case FCMD_RD1PHR:
        /* Verify-erased: FAIL if any byte in scope is not 0xFF. */
        if (flash) {
            uint64_t len = (cmd == FCMD_RD1SCR) ? MCXN_FLASH_SECTOR :
                           (cmd == FCMD_RD1PHR) ? 16 : s->flash_size;
            off = (cmd == FCMD_RD1ALL) ? 0 : fmu_flash_off(s, addr);
            for (uint64_t i = 0; i < len && off + i < s->flash_size; i++) {
                if (flash[off + i] != 0xFF) {
                    s->fstat |= FSTAT_FAIL;
                    break;
                }
            }
        }
        break;
    default:
        s->fstat |= FSTAT_ACCERR;   /* unrecognized command */
        break;
    }
    s->fstat |= FSTAT_CCIF;         /* command complete */
    fmu_update_irq(s);
}

static uint64_t mcxn_fmu_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNFMUState *s = MCXN_FMU(opaque);

    switch (off) {
    case R_FSTAT: return s->fstat;
    case R_FCNFG: return s->fcnfg;
    case R_FCTRL: return s->fctrl;
    default:
        if (off >= R_FCCOB0 && off < R_FCCOB0 + 8 * 4) {
            return s->fccob[(off - R_FCCOB0) / 4];
        }
        return 0;
    }
}

static void mcxn_fmu_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MCXNFMUState *s = MCXN_FMU(opaque);
    uint32_t v = val;

    switch (off) {
    case R_FSTAT:
        /* Writing 1 to CCIF launches the staged command; the error flags are
         * write-1-to-clear. */
        s->fstat &= ~(v & (FSTAT_FAIL | FSTAT_ACCERR | FSTAT_PEWERR |
                           FSTAT_CMDABT));
        if (v & FSTAT_CCIF) {
            fmu_launch(s);
        }
        return;
    case R_FCNFG:
        s->fcnfg = v;
        fmu_update_irq(s);
        return;
    case R_FCTRL:
        s->fctrl = v;
        return;
    default:
        if (off >= R_FCCOB0 && off < R_FCCOB0 + 8 * 4) {
            s->fccob[(off - R_FCCOB0) / 4] = v;
        }
        return;
    }
}

static const MemoryRegionOps mcxn_fmu_ops = {
    .read = mcxn_fmu_read,
    .write = mcxn_fmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_fmu_reset(DeviceState *dev)
{
    MCXNFMUState *s = MCXN_FMU(dev);

    s->fstat = FSTAT_CCIF;          /* idle, command complete */
    s->fcnfg = 0;
    s->fctrl = 0;
    memset(s->fccob, 0, sizeof(s->fccob));
}

static void mcxn_fmu_realize(DeviceState *dev, Error **errp)
{
    MCXNFMUState *s = MCXN_FMU(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_fmu_ops, s,
                          TYPE_MCXN_FMU, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_fmu = {
    .name = TYPE_MCXN_FMU,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fstat, MCXNFMUState),
        VMSTATE_UINT32(fcnfg, MCXNFMUState),
        VMSTATE_UINT32(fctrl, MCXNFMUState),
        VMSTATE_UINT32_ARRAY(fccob, MCXNFMUState, 8),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_fmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_fmu_realize;
    device_class_set_legacy_reset(dc, mcxn_fmu_reset);
    dc->vmsd = &vmstate_mcxn_fmu;
}

static const TypeInfo mcxn_fmu_types[] = {
    {
        .name          = TYPE_MCXN_FMU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNFMUState),
        .class_init    = mcxn_fmu_class_init,
    },
};

DEFINE_TYPES(mcxn_fmu_types)
