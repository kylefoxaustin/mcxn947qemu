/*
 * NXP MCX N I3C (Improved Inter-Integrated Circuit) — bring-up model.
 *
 * Models both I3C0 and I3C1 with a single type.  The block has a controller
 * register bank (M-prefixed) and a target register bank (S-prefixed).  Firmware
 * polls FIFO fullness/emptiness and various status bits before transferring; the
 * model seeds every register with the reset value documented in the MCXN947
 * reference manual and pins the FIFO/idle status bits so those polls always
 * complete.  Write-1-to-clear is applied to the error/warning status registers.
 *
 * Read-only identification registers (SCAPABILITIES, SCAPABILITIES2, SID) return
 * the RM reset constants.  Offsets come from the MCXN947 CMSIS header (I3C_Type);
 * reset values come from RM chapter 72 (I3C register descriptions).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_i3c.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS I3C_Type). */
#define R_MCONFIG        0x000  /* RW controller config */
#define R_SCONFIG        0x004  /* RW target config */
#define R_SSTATUS        0x008  /* status, some W1C */
#define R_SCTRL          0x00C
#define R_SINTSET        0x010
#define R_SINTCLR        0x014
#define R_SINTMASKED     0x018  /* RO */
#define R_SERRWARN       0x01C  /* W1C */
#define R_SDMACTRL       0x020
#define R_SDATACTRL      0x02C  /* FIFO control + RXEMPTY/TXFULL status */
#define R_SWDATAB        0x030  /* WO */
#define R_SWDATABE       0x034  /* WO */
#define R_SWDATAH        0x038  /* WO */
#define R_SWDATAHE       0x03C  /* WO */
#define R_SRDATAB        0x040  /* RO */
#define R_SRDATAH        0x048  /* RO */
#define R_SWDATAB1       0x054  /* WO */
#define R_SCAPABILITIES2 0x05C  /* RO */
#define R_SCAPABILITIES  0x060  /* RO */
#define R_SDYNADDR       0x064
#define R_SMAXLIMITS     0x068
#define R_SIDPARTNO      0x06C
#define R_SIDEXT         0x070
#define R_SVENDORID      0x074
#define R_STCCLOCK       0x078
#define R_SMSGMAPADDR    0x07C  /* RO */
#define R_MCTRL          0x084
#define R_MSTATUS        0x088  /* status, some W1C */
#define R_MIBIRULES      0x08C
#define R_MINTSET        0x090
#define R_MINTCLR        0x094
#define R_MINTMASKED     0x098  /* RO */
#define R_MERRWARN       0x09C  /* W1C */
#define R_MDMACTRL       0x0A0
#define R_MDATACTRL      0x0AC  /* FIFO control + RXEMPTY/TXFULL status */
#define R_MWDATAB        0x0B0  /* WO */
#define R_MWDATABE       0x0B4  /* WO */
#define R_MWDATAH        0x0B8  /* WO */
#define R_MWDATAHE       0x0BC  /* WO */
#define R_MRDATAB        0x0C0  /* RO */
#define R_MRDATAH        0x0C8  /* RO */
#define R_MWDATAB1       0x0CC  /* WO */
#define R_MWMSG_SDR      0x0D0  /* WO */
#define R_MRMSG_SDR      0x0D4  /* RO */
#define R_MWMSG_DDR      0x0D8  /* WO */
#define R_MRMSG_DDR      0x0DC  /* RO */
#define R_MDYNADDR       0x0E4
#define R_SMAPCTRL0      0x11C  /* RO */
#define R_IBIEXT1        0x140
#define R_IBIEXT2        0x144
#define R_SID            0xFFC  /* RO module ID */

/*
 * Reset values from RM chapter 72.  The FIFO-control registers reset with
 * RXEMPTY=1 (bit 31) and TXFULL=0 (bit 30), i.e. receive empty and transmit not
 * full, which is exactly the idle state firmware waits for before it reads or
 * writes data, so they need no special handling beyond keeping those bits stuck.
 */
#define I3C_SCONFIG_RST        0x00010000u
#define I3C_SSTATUS_RST        0x00001400u
#define I3C_SDMACTRL_RST       0x00000010u
#define I3C_SDATACTRL_RST      0x80000030u
#define I3C_SCAPABILITIES2_RST 0x00000300u
#define I3C_SCAPABILITIES_RST  0xE83FFE78u
#define I3C_SIDPARTNO_RST      0x30000000u
#define I3C_SIDEXT_RST         0x0000EF00u
#define I3C_SVENDORID_RST      0x0000011Bu
#define I3C_STCCLOCK_RST       0x00000214u
#define I3C_MSTATUS_RST        0x00001000u
#define I3C_MDMACTRL_RST       0x00000010u
#define I3C_MDATACTRL_RST      0x80000030u
#define I3C_IBIEXT1_RST        0x00000070u
#define I3C_SID_RST            0xEDCB0100u

/* FIFO status bits inside the *DATACTRL registers (RM: bit 31/30). */
#define I3C_DATACTRL_RXEMPTY   (1u << 31)
#define I3C_DATACTRL_TXFULL    (1u << 30)
#define I3C_DATACTRL_FIFO_MASK (I3C_DATACTRL_RXEMPTY | I3C_DATACTRL_TXFULL)

static void mcxn_i3c_reset(DeviceState *dev)
{
    MCXNI3CState *s = MCXN_I3C(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_SCONFIG / 4]        = I3C_SCONFIG_RST;
    s->regs[R_SSTATUS / 4]        = I3C_SSTATUS_RST;
    s->regs[R_SDMACTRL / 4]       = I3C_SDMACTRL_RST;
    s->regs[R_SDATACTRL / 4]      = I3C_SDATACTRL_RST;
    s->regs[R_SCAPABILITIES2 / 4] = I3C_SCAPABILITIES2_RST;
    s->regs[R_SCAPABILITIES / 4]  = I3C_SCAPABILITIES_RST;
    s->regs[R_SIDPARTNO / 4]      = I3C_SIDPARTNO_RST;
    s->regs[R_SIDEXT / 4]         = I3C_SIDEXT_RST;
    s->regs[R_SVENDORID / 4]      = I3C_SVENDORID_RST;
    s->regs[R_STCCLOCK / 4]       = I3C_STCCLOCK_RST;
    s->regs[R_MSTATUS / 4]        = I3C_MSTATUS_RST;
    s->regs[R_MDMACTRL / 4]       = I3C_MDMACTRL_RST;
    s->regs[R_MDATACTRL / 4]      = I3C_MDATACTRL_RST;
    s->regs[R_IBIEXT1 / 4]        = I3C_IBIEXT1_RST;
    s->regs[R_SID / 4]            = I3C_SID_RST;
    qemu_set_irq(s->irq, 0);
}

/* MCTRL request field and the controller status bits it completes. */
#define I3C_MCTRL_REQUEST    0x7u
#define I3C_MSTATUS_MCTRLDONE 0x200u
#define I3C_MSTATUS_COMPLETE  0x400u

/*
 * Controller interrupt: MINTMASKED = MSTATUS & enabled (MINTSET holds the
 * enable mask; MINTCLR clears it).  The masked status is cached so its
 * read-only register reflects it, and it drives the IRQ line.
 */
static void mcxn_i3c_update_irq(MCXNI3CState *s)
{
    uint32_t pend = s->regs[R_MSTATUS / 4] & s->regs[R_MINTSET / 4];

    s->regs[R_MINTMASKED / 4] = pend;
    qemu_set_irq(s->irq, pend != 0);
}

static bool i3c_is_readonly(hwaddr off)
{
    switch (off) {
    case R_SINTMASKED:
    case R_SRDATAB:
    case R_SRDATAH:
    case R_SCAPABILITIES2:
    case R_SCAPABILITIES:
    case R_SMSGMAPADDR:
    case R_MINTMASKED:
    case R_MRDATAB:
    case R_MRDATAH:
    case R_MRMSG_SDR:
    case R_MRMSG_DDR:
    case R_SMAPCTRL0:
    case R_SID:
        return true;
    default:
        return false;
    }
}

static uint64_t mcxn_i3c_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNI3CState *s = MCXN_I3C(opaque);
    hwaddr idx = off & ~0x3u;
    uint32_t reg, shift;

    if (off >= MCXN_I3C_SIZE) {
        return 0;
    }
    reg = s->regs[idx >> 2];

    /* Keep the FIFO status bits pinned to the idle state so polled firmware
     * (RXEMPTY=1, TXFULL=0) always sees room to write and a "done" on read. */
    if (idx == R_SDATACTRL || idx == R_MDATACTRL) {
        reg = (reg & ~I3C_DATACTRL_FIFO_MASK) | I3C_DATACTRL_RXEMPTY;
    }

    shift = (off & 0x3u) * 8;
    return (reg >> shift) & ((size == 4) ? 0xFFFFFFFFu
                                         : ((1u << (size * 8)) - 1));
}

static void mcxn_i3c_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    MCXNI3CState *s = MCXN_I3C(opaque);
    hwaddr idx = off & ~0x3u;
    uint32_t shift, mask, cur, v;

    if (off >= MCXN_I3C_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (i3c_is_readonly(idx)) {
        return;
    }

    /* Expand a byte/halfword write into the 32-bit backing word. */
    shift = (off & 0x3u) * 8;
    mask = ((size == 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1)) << shift;
    cur = s->regs[idx >> 2];
    v = (cur & ~mask) | ((uint32_t)(value << shift) & mask);

    /* Error/warning status registers are write-1-to-clear: only the bits
     * written as 1 are cleared, the rest are preserved. */
    if (idx == R_SERRWARN || idx == R_MERRWARN) {
        uint32_t w1c = (uint32_t)(value << shift) & mask;
        s->regs[idx >> 2] = cur & ~w1c;
        return;
    }

    /* SSTATUS / MSTATUS carry a mix of W1C flags and live status.  Treat the
     * bits being written as 1 as clears; preserve everything else (including
     * the idle/state field reset constants). */
    if (idx == R_SSTATUS || idx == R_MSTATUS) {
        uint32_t w1c = (uint32_t)(value << shift) & mask;
        s->regs[idx >> 2] = cur & ~w1c;
        if (idx == R_MSTATUS) {
            mcxn_i3c_update_irq(s);
        }
        return;
    }

    /* MINTSET is write-1-to-set, MINTCLR write-1-to-clear, of the controller
     * interrupt-enable mask (held in MINTSET). */
    if (idx == R_MINTSET) {
        s->regs[R_MINTSET / 4] |= (uint32_t)(value << shift) & mask;
        mcxn_i3c_update_irq(s);
        return;
    }
    if (idx == R_MINTCLR) {
        s->regs[R_MINTSET / 4] &= ~((uint32_t)(value << shift) & mask);
        s->regs[R_MINTCLR / 4] = v;
        mcxn_i3c_update_irq(s);
        return;
    }

    /* Issuing a controller request (MCTRL.REQUEST != 0) completes the message
     * immediately: raise MCTRLDONE + COMPLETE so a polled or interrupt-driven
     * transfer resolves. */
    if (idx == R_MCTRL) {
        s->regs[idx >> 2] = v;
        if (v & I3C_MCTRL_REQUEST) {
            s->regs[R_MSTATUS / 4] |= I3C_MSTATUS_MCTRLDONE | I3C_MSTATUS_COMPLETE;
            mcxn_i3c_update_irq(s);
        }
        return;
    }

    /* Never let firmware mark the FIFO as non-idle. */
    if (idx == R_SDATACTRL || idx == R_MDATACTRL) {
        v = (v & ~I3C_DATACTRL_FIFO_MASK) | I3C_DATACTRL_RXEMPTY;
    }

    s->regs[idx >> 2] = v;
}

static const MemoryRegionOps mcxn_i3c_ops = {
    .read = mcxn_i3c_read,
    .write = mcxn_i3c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_i3c_realize(DeviceState *dev, Error **errp)
{
    MCXNI3CState *s = MCXN_I3C(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_i3c_ops, s,
                          TYPE_MCXN_I3C, MCXN_I3C_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_i3c = {
    .name = TYPE_MCXN_I3C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNI3CState, MCXN_I3C_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_i3c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_i3c_realize;
    device_class_set_legacy_reset(dc, mcxn_i3c_reset);
    dc->vmsd = &vmstate_mcxn_i3c;
}

static const TypeInfo mcxn_i3c_types[] = {
    {
        .name          = TYPE_MCXN_I3C,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNI3CState),
        .class_init    = mcxn_i3c_class_init,
    },
};

DEFINE_TYPES(mcxn_i3c_types)
