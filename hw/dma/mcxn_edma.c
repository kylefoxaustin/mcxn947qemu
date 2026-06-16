/*
 * NXP MCX N eDMA (enhanced DMA) — functional model.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/dma/mcxn_edma.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* Management-page registers. */
#define R_MP_CSR  0x00
#define R_MP_ES   0x04
#define R_MP_INT  0x08
#define R_MP_HRS  0x0C
#define R_CH_GRPRI 0x100   /* [16] */

/* Per-channel register offsets (within the channel's 0x1000 block). */
#define R_CH_CSR   0x00
#define R_CH_ES    0x04
#define R_CH_INT   0x08
#define R_CH_SBR   0x0C
#define R_CH_PRI   0x10
#define R_CH_MUX   0x14
#define R_TCD_SADDR 0x20
#define R_TCD_SOFF  0x24
#define R_TCD_ATTR  0x26
#define R_TCD_NBYTES 0x28
#define R_TCD_SLAST 0x2C
#define R_TCD_DADDR 0x30
#define R_TCD_DOFF  0x34
#define R_TCD_CITER 0x36
#define R_TCD_DLAST 0x38
#define R_TCD_CSR   0x3C
#define R_TCD_BITER 0x3E

#define CH_CSR_ERQ   (1u << 0)
#define CH_CSR_DONE  (1u << 30)
#define CH_INT_INT   (1u << 0)
#define TCD_CSR_START    (1u << 0)
#define TCD_CSR_INTMAJOR (1u << 1)
#define ATTR_SSIZE(a)  (((a) >> 8) & 0x7)
#define ATTR_DSIZE(a)  ((a) & 0x7)
#define NBYTES_MASK  0x3FFFFFFFu
#define CITER_MASK   0x7FFFu

static void edma_update_irq(MCXNEDMAState *s, int n)
{
    qemu_set_irq(s->irq[n], !!(s->ch[n].intr & CH_INT_INT));
}

/* Run a software-triggered channel transfer to completion. */
static void edma_run(MCXNEDMAState *s, int n)
{
    MCXNEDMAChan *c = &s->ch[n];
    uint32_t ssize = 1u << ATTR_SSIZE(c->tcd_attr);
    uint32_t dsize = 1u << ATTR_DSIZE(c->tcd_attr);
    uint32_t nbytes = c->tcd_nbytes & NBYTES_MASK;
    uint32_t citer = c->tcd_citer & CITER_MASK;
    int16_t soff = (int16_t)c->tcd_soff;
    int16_t doff = (int16_t)c->tcd_doff;
    uint32_t saddr = c->tcd_saddr, daddr = c->tcd_daddr;
    uint8_t buf[32];
    uint32_t step = ssize ? ssize : 1;
    uint32_t m, b;

    if (dsize == 0 || nbytes == 0 || citer == 0) {
        c->csr |= CH_CSR_DONE;
        return;
    }
    if (step > sizeof(buf)) {
        step = sizeof(buf);
    }
    for (m = 0; m < citer; m++) {
        for (b = 0; b + step <= nbytes; b += step) {
            address_space_read(&address_space_memory, saddr,
                               MEMTXATTRS_UNSPECIFIED, buf, step);
            address_space_write(&address_space_memory, daddr,
                                MEMTXATTRS_UNSPECIFIED, buf, step);
            saddr += soff;
            daddr += doff;
        }
    }
    saddr += (int32_t)c->tcd_slast;
    daddr += (int32_t)c->tcd_dlast;
    c->tcd_saddr = saddr;
    c->tcd_daddr = daddr;
    c->tcd_citer = c->tcd_biter;          /* reload major count */
    c->csr |= CH_CSR_DONE;
    if (c->tcd_csr & TCD_CSR_INTMAJOR) {
        c->intr |= CH_INT_INT;
        edma_update_irq(s, n);
    }
}

static uint64_t edma_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNEDMAState *s = MCXN_EDMA(opaque);
    MCXNEDMAChan *c;
    int n;

    if (off < 0x1000) {
        switch (off) {
        case R_MP_CSR: return s->mp_csr;
        case R_MP_ES:  return s->mp_es;
        case R_MP_INT: {
            uint32_t r = 0;
            for (n = 0; n < MCXN_EDMA_CHANNELS; n++) {
                if (s->ch[n].intr & CH_INT_INT) {
                    r |= (1u << n);
                }
            }
            return r;
        }
        case R_MP_HRS: return 0;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * MCXN_EDMA_CHANNELS) {
                return s->ch_grpri[(off - R_CH_GRPRI) / 4];
            }
            return 0;
        }
    }

    n = (off / 0x1000) - 1;
    if (n >= MCXN_EDMA_CHANNELS) {
        return 0;
    }
    c = &s->ch[n];
    switch (off % 0x1000) {
    case R_CH_CSR:    return c->csr;
    case R_CH_ES:     return c->es;
    case R_CH_INT:    return c->intr;
    case R_CH_SBR:    return c->sbr;
    case R_CH_PRI:    return c->pri;
    case R_CH_MUX:    return c->mux;
    case R_TCD_SADDR: return c->tcd_saddr;
    case R_TCD_SOFF:  return c->tcd_soff;
    case R_TCD_ATTR:  return c->tcd_attr;
    case R_TCD_NBYTES: return c->tcd_nbytes;
    case R_TCD_SLAST: return c->tcd_slast;
    case R_TCD_DADDR: return c->tcd_daddr;
    case R_TCD_DOFF:  return c->tcd_doff;
    case R_TCD_CITER: return c->tcd_citer;
    case R_TCD_DLAST: return c->tcd_dlast;
    case R_TCD_CSR:   return c->tcd_csr;
    case R_TCD_BITER: return c->tcd_biter;
    default:          return 0;
    }
}

static void edma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MCXNEDMAState *s = MCXN_EDMA(opaque);
    MCXNEDMAChan *c;
    uint32_t v = val;
    int n;

    if (off < 0x1000) {
        switch (off) {
        case R_MP_CSR: s->mp_csr = v; return;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * MCXN_EDMA_CHANNELS) {
                s->ch_grpri[(off - R_CH_GRPRI) / 4] = v;
            }
            return;
        }
    }

    n = (off / 0x1000) - 1;
    if (n >= MCXN_EDMA_CHANNELS) {
        return;
    }
    c = &s->ch[n];
    switch (off % 0x1000) {
    case R_CH_CSR:
        /* DONE is write-1-to-clear; keep the rest. */
        if (v & CH_CSR_DONE) {
            c->csr &= ~CH_CSR_DONE;
        }
        c->csr = (c->csr & CH_CSR_DONE) | (v & ~CH_CSR_DONE);
        return;
    case R_CH_INT:
        if (v & CH_INT_INT) {                 /* write-1-to-clear */
            c->intr &= ~CH_INT_INT;
            edma_update_irq(s, n);
        }
        return;
    case R_CH_ES:   c->es = v; return;
    case R_CH_SBR:  c->sbr = v; return;
    case R_CH_PRI:  c->pri = v; return;
    case R_CH_MUX:  c->mux = v; return;
    case R_TCD_SADDR: c->tcd_saddr = v; return;
    case R_TCD_SOFF:  c->tcd_soff = v; return;
    case R_TCD_ATTR:  c->tcd_attr = v; return;
    case R_TCD_NBYTES: c->tcd_nbytes = v; return;
    case R_TCD_SLAST: c->tcd_slast = v; return;
    case R_TCD_DADDR: c->tcd_daddr = v; return;
    case R_TCD_DOFF:  c->tcd_doff = v; return;
    case R_TCD_CITER: c->tcd_citer = v; return;
    case R_TCD_DLAST: c->tcd_dlast = v; return;
    case R_TCD_CSR:
        c->tcd_csr = v;
        if (v & TCD_CSR_START) {
            c->csr &= ~CH_CSR_DONE;
            edma_run(s, n);
        }
        return;
    case R_TCD_BITER: c->tcd_biter = v; return;
    default: return;
    }
}

static const MemoryRegionOps edma_ops = {
    .read = edma_read,
    .write = edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_edma_reset(DeviceState *dev)
{
    MCXNEDMAState *s = MCXN_EDMA(dev);

    s->mp_csr = 0;
    s->mp_es = 0;
    memset(s->ch_grpri, 0, sizeof(s->ch_grpri));
    memset(s->ch, 0, sizeof(s->ch));
}

static void mcxn_edma_realize(DeviceState *dev, Error **errp)
{
    MCXNEDMAState *s = MCXN_EDMA(dev);
    int n;

    /* Management page (0x0) + 16 channels x 0x1000 = 0x11000. */
    memory_region_init_io(&s->iomem, OBJECT(s), &edma_ops, s, TYPE_MCXN_EDMA,
                          0x1000 * (MCXN_EDMA_CHANNELS + 1));
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (n = 0; n < MCXN_EDMA_CHANNELS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[n]);
    }
}

static const VMStateDescription vmstate_edma_chan = {
    .name = "mcxn-edma-chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(csr, MCXNEDMAChan),
        VMSTATE_UINT32(es, MCXNEDMAChan),
        VMSTATE_UINT32(intr, MCXNEDMAChan),
        VMSTATE_UINT32(sbr, MCXNEDMAChan),
        VMSTATE_UINT32(pri, MCXNEDMAChan),
        VMSTATE_UINT32(mux, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_saddr, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_slast, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_daddr, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_dlast, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_nbytes, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_soff, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_attr, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_doff, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_citer, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_csr, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_biter, MCXNEDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_mcxn_edma = {
    .name = TYPE_MCXN_EDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mp_csr, MCXNEDMAState),
        VMSTATE_UINT32(mp_es, MCXNEDMAState),
        VMSTATE_UINT32_ARRAY(ch_grpri, MCXNEDMAState, MCXN_EDMA_CHANNELS),
        VMSTATE_STRUCT_ARRAY(ch, MCXNEDMAState, MCXN_EDMA_CHANNELS, 1,
                             vmstate_edma_chan, MCXNEDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_edma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_edma_realize;
    device_class_set_legacy_reset(dc, mcxn_edma_reset);
    dc->vmsd = &vmstate_mcxn_edma;
}

static const TypeInfo mcxn_edma_types[] = {
    {
        .name          = TYPE_MCXN_EDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEDMAState),
        .class_init    = mcxn_edma_class_init,
    },
};

DEFINE_TYPES(mcxn_edma_types)
