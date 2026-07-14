/*
 * NXP MCX N PDM / MICFIL (digital microphone interface) — honest model.  See
 * header for why an empty-but-LOUD filter is the correct model here, and why a
 * silent zero-stream is not.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_pdm.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* --- Register offsets ------------------------------------------------------ */
#define PDM_CTRL_1      0x00
#define PDM_CTRL_2      0x04
#define PDM_STAT        0x08  /* BSY_FIL + W1C channel flags */
#define PDM_FIFO_CTRL   0x10
#define PDM_FIFO_STAT   0x14  /* W1C over/underflow flags */
#define PDM_DATACH0     0x24  /* RO output result, 4 x 0x4 */
#define PDM_DATACH3     0x30
#define PDM_DC_CTRL     0x64  /* RO */
#define PDM_RANGE_STAT  0x7C  /* W1C range flags */
#define PDM_VERID       0x84  /* RO */
#define PDM_PARAM       0x88  /* RO */

/* --- CTRL_1 (CMSIS PDM_CTRL_1_*) ------------------------------------------- */
#define CTRL1_CHEN(n)   (1u << (n))     /* CH0EN..CH3EN */
#define CTRL1_CHEN_ALL  0x0Fu
#define CTRL1_ERREN     (1u << 23)
#define CTRL1_DISEL     (3u << 24)      /* 00 off, 01 DMA, 10 IRQ */
#define CTRL1_DISEL_IRQ (2u << 24)
#define CTRL1_SRES      (1u << 27)      /* software reset */
#define CTRL1_PDMIEN    (1u << 29)      /* filter enable */

/* --- STAT ------------------------------------------------------------------ */
#define STAT_CHF(n)     (1u << (n))     /* watermark reached, W1C */
#define STAT_CHF_ALL    0x0Fu
#define STAT_BSY_FIL    0x80000000u     /* filter running */

/* --- FIFO_CTRL / FIFO_STAT ------------------------------------------------- */
#define FIFO_CTRL_WMK   0x0Fu
#define FIFO_OVF(n)     (1u << (n))
#define FIFO_UND(n)     (1u << (8 + (n)))

/*
 * Identity registers.  Reset values decoded from the RM register tables
 * (§76.7.13 VERID, §76.7.14 PARAM) — not invented:
 *   VERID = MAJOR 2, MINOR 0x0F
 *   PARAM = NPAIR 2 (4 channels), FIFO_PTRWID 4 (16-deep), 24-bit out,
 *           LOW_POWER, DC_BYPASS
 */
#define PDM_VERID_VALUE  0x020F0000u
#define PDM_PARAM_VALUE  0x00000742u

static bool pdm_is_datach(hwaddr offset)
{
    return offset >= PDM_DATACH0 && offset <= PDM_DATACH3;
}

static void pdm_update_irq(MCXNPDMState *s)
{
    uint32_t ctrl1 = s->regs[PDM_CTRL_1 / 4];
    uint32_t stat  = s->regs[PDM_STAT / 4];
    uint32_t fstat = s->regs[PDM_FIFO_STAT / 4];
    bool level = false;

    /* A watermark event only raises an interrupt when DISEL selects IRQ. */
    if ((ctrl1 & CTRL1_DISEL) == CTRL1_DISEL_IRQ) {
        level |= (stat & STAT_CHF_ALL) != 0;
    }
    if (ctrl1 & CTRL1_ERREN) {
        level |= fstat != 0;
    }
    qemu_set_irq(s->irq, level);
}

static void pdm_flush_fifos(MCXNPDMState *s)
{
    memset(s->fifo_count, 0, sizeof(s->fifo_count));
    s->regs[PDM_STAT / 4] &= ~STAT_CHF_ALL;
    s->regs[PDM_FIFO_STAT / 4] = 0;
}

static uint64_t mcxn_pdm_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNPDMState *s = MCXN_PDM(opaque);

    switch (offset) {
    case PDM_VERID:
        return PDM_VERID_VALUE;
    case PDM_PARAM:
        return PDM_PARAM_VALUE;
    case PDM_STAT: {
        uint32_t stat = s->regs[PDM_STAT / 4] & STAT_CHF_ALL;

        /* BSY_FIL tracks the filter actually running; RM §76.7.4 makes it the
         * documented way to confirm the filters have stopped before a mode
         * change. */
        if (s->regs[PDM_CTRL_1 / 4] & CTRL1_PDMIEN) {
            stat |= STAT_BSY_FIL;
        }
        return stat;
    }
    case PDM_DC_CTRL:
        return s->regs[offset / 4];
    default:
        break;
    }

    if (pdm_is_datach(offset)) {
        int n = (offset - PDM_DATACH0) / 4;

        if (n < MCXN_PDM_NUM_CH && s->fifo_count[n] > 0) {
            uint32_t v = s->fifo[n][0];

            memmove(&s->fifo[n][0], &s->fifo[n][1],
                    (--s->fifo_count[n]) * sizeof(s->fifo[n][0]));
            if (s->fifo_count[n] <=
                (s->regs[PDM_FIFO_CTRL / 4] & FIFO_CTRL_WMK)) {
                s->regs[PDM_STAT / 4] &= ~STAT_CHF(n);
                pdm_update_irq(s);
            }
            return v;
        }

        /*
         * Nothing to read.  Flag the underflow rather than handing back a
         * plausible zero: a mute microphone must not be indistinguishable from
         * a working one.
         */
        if (n < MCXN_PDM_NUM_CH) {
            s->regs[PDM_FIFO_STAT / 4] |= FIFO_UND(n);
            pdm_update_irq(s);
        }
        return 0;
    }

    return s->regs[offset / 4];
}

static void mcxn_pdm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNPDMState *s = MCXN_PDM(opaque);
    uint32_t v = value;

    switch (offset) {
    case PDM_VERID:
    case PDM_PARAM:
    case PDM_DC_CTRL:
        return;  /* read-only */

    case PDM_CTRL_1:
        if (v & CTRL1_SRES) {
            memset(s->regs, 0, sizeof(s->regs));
            pdm_flush_fifos(s);
            pdm_update_irq(s);
            return;                  /* SRES is self-clearing */
        }
        s->regs[PDM_CTRL_1 / 4] = v;

        if ((v & CTRL1_PDMIEN) && (v & CTRL1_CHEN_ALL) &&
            !s->warned_no_source) {
            s->warned_no_source = true;
            qemu_log_mask(LOG_UNIMP,
                "mcxn-pdm: MICFIL enabled with no audio source attached.  The "
                "PDM microphone bitstream has no source in emulation, so NO "
                "samples will be produced: STAT[CHnF] will not set and reads of "
                "DATACHn will report FIFO_STAT[FIFOUNDn] underflow.  (The "
                "filter is deliberately not fabricating silence, which firmware "
                "could not tell apart from real audio.)\n");
        }
        pdm_update_irq(s);
        return;

    case PDM_STAT:
        s->regs[PDM_STAT / 4] &= ~(v & STAT_CHF_ALL);   /* W1C */
        pdm_update_irq(s);
        return;

    case PDM_FIFO_STAT:
    case PDM_RANGE_STAT:
        s->regs[offset / 4] &= ~v;                      /* W1C */
        pdm_update_irq(s);
        return;

    case PDM_FIFO_CTRL:
        s->regs[PDM_FIFO_CTRL / 4] = v;
        pdm_update_irq(s);
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
     * eDMA byte-access lesson).  The write default merges sub-word so no config
     * register is corrupted.
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
    /* RM reset (derived, never invented).  A zero watermark IS a watermark. */
    s->regs[0x010 / 4] = 0x0000000Fu;   /* FIFO_CTRL  watermark */
    s->regs[0x064 / 4] = 0x000000FFu;   /* DC_CTRL */
    memset(s->fifo, 0, sizeof(s->fifo));
    pdm_flush_fifos(s);
    s->warned_no_source = false;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_pdm_realize(DeviceState *dev, Error **errp)
{
    MCXNPDMState *s = MCXN_PDM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_pdm_ops, s,
                          TYPE_MCXN_PDM, MCXN_PDM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_pdm = {
    .name = TYPE_MCXN_PDM,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPDMState, MCXN_PDM_SIZE / 4),
        VMSTATE_UINT32_2DARRAY(fifo, MCXNPDMState, MCXN_PDM_NUM_CH,
                               MCXN_PDM_FIFO_DEPTH),
        VMSTATE_UINT32_ARRAY(fifo_count, MCXNPDMState, MCXN_PDM_NUM_CH),
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
