/*
 * NXP MCX N PDM / MICFIL (digital microphone interface) — bring-up model.
 *
 * No real microphone: the filter never reports busy (STAT.BSY_FIL reads 0), the
 * per-channel output FIFOs read empty so reads return 0, and the FIFO status
 * over/underflow flags are W1C.  Bit masks and offsets taken verbatim from the
 * MCXN947 CMSIS header (PDM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_pdm.h"
#include "migration/vmstate.h"

/* --- Register offsets ------------------------------------------------------ */
#define PDM_CTRL_1      0x00
#define PDM_CTRL_2      0x04
#define PDM_STAT        0x08  /* status (BSY_FIL + W1C channel flags) */
#define PDM_FIFO_CTRL   0x10
#define PDM_FIFO_STAT   0x14  /* W1C over/underflow flags */
#define PDM_DATACH0     0x24  /* RO output result, 4 x 0x4 */
#define PDM_DATACH3     0x30
#define PDM_DC_CTRL     0x64  /* RO */
#define PDM_DC_OUT_CTRL 0x68
#define PDM_RANGE_CTRL  0x74
#define PDM_RANGE_STAT  0x7C  /* W1C range flags */
#define PDM_FSYNC_CTRL  0x80
#define PDM_VERID       0x84  /* RO */
#define PDM_PARAM       0x88  /* RO */

/* --- STAT bit masks (CMSIS) ------------------------------------------------ */
#define STAT_BSY_FIL    0x80000000u  /* filter busy */

/*
 * VERID/PARAM are read-only identity registers.  Plausible MCX-class
 * constants; refine against the RM if a HAL depends on them.
 */
#define PDM_VERID_VALUE  0x01000000u
#define PDM_PARAM_VALUE  0x00000104u  /* NPAIR + 24-bit output width */

static bool pdm_is_datach(hwaddr offset)
{
    return offset >= PDM_DATACH0 && offset <= PDM_DATACH3;
}

static uint64_t mcxn_pdm_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNPDMState *s = MCXN_PDM(opaque);

    switch (offset) {
    case PDM_VERID:
        return PDM_VERID_VALUE;
    case PDM_PARAM:
        return PDM_PARAM_VALUE;
    case PDM_STAT:
        /* Never busy; preserve any latched (W1C) channel flags. */
        return s->regs[PDM_STAT / 4] & ~STAT_BSY_FIL;
    case PDM_DC_CTRL:
        return s->regs[offset / 4];  /* RO but software-visible value is 0 */
    default:
        break;
    }

    if (pdm_is_datach(offset)) {
        return 0;  /* RO; output FIFO empty, no captured audio */
    }

    return s->regs[offset / 4];
}

static void mcxn_pdm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNPDMState *s = MCXN_PDM(opaque);

    switch (offset) {
    case PDM_VERID:
    case PDM_PARAM:
    case PDM_DC_CTRL:
        return;  /* read-only */
    case PDM_STAT:
    case PDM_FIFO_STAT:
    case PDM_RANGE_STAT:
        /* W1C status registers. */
        s->regs[offset / 4] &= ~(uint32_t)value;
        return;
    default:
        break;
    }

    if (pdm_is_datach(offset)) {
        return;  /* read-only */
    }

    /* Merge sub-word writes so a byte/halfword access can't clobber the other
     * bytes of a config register (see the access-size note on the ops). */
    {
        uint32_t idx = offset / 4;
        uint32_t shift = (offset & 3) * 8;
        uint32_t mask = (size >= 4) ? 0xFFFFFFFFu
                                    : (((1u << (size * 8)) - 1) << shift);
        s->regs[idx] = (s->regs[idx] & ~mask) |
                       ((uint32_t)(value << shift) & mask);
    }
}

static const MemoryRegionOps mcxn_pdm_ops = {
    .read = mcxn_pdm_read,
    .write = mcxn_pdm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * Accept 1/2/4-byte access: PDM output FIFOs are drained over eDMA, which
     * can burst sub-word reads; a 4-byte-only window would reject them (fleet
     * eDMA byte-access lesson).  Data channels are RO (read 0 — no captured
     * audio); the write default merges sub-word so no config reg is corrupted.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_pdm_reset(DeviceState *dev)
{
    MCXNPDMState *s = MCXN_PDM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_pdm_realize(DeviceState *dev, Error **errp)
{
    MCXNPDMState *s = MCXN_PDM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_pdm_ops, s,
                          TYPE_MCXN_PDM, MCXN_PDM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_pdm = {
    .name = TYPE_MCXN_PDM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPDMState, MCXN_PDM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_pdm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_pdm_realize;
    device_class_set_legacy_reset(dc, mcxn_pdm_reset);
    dc->vmsd = &vmstate_mcxn_pdm;
}

static const TypeInfo mcxn_pdm_types[] = {
    {
        .name          = TYPE_MCXN_PDM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPDMState),
        .class_init    = mcxn_pdm_class_init,
    },
};

DEFINE_TYPES(mcxn_pdm_types)
