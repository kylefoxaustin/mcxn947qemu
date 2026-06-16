/*
 * NXP MCX N FlexCAN (Flexible Controller Area Network, CAN FD) — bring-up model.
 *
 * The bring-up blockers are the FlexCAN mode handshakes in MCR (Module
 * Configuration):
 *
 *   - On reset the module is disabled and not ready: MDIS=1, FRZ=1, HALT=1,
 *     NOTRDY=1, FRZACK=1 (RM: "When the module is enabled (MDIS becomes 0),
 *     FlexCAN automatically enters Freeze mode" with HALT/FRZ/FRZACK/NOTRDY=1).
 *   - When firmware enters Freeze mode (FRZ=1 and HALT=1, or MDIS=1) the model
 *     asserts FRZACK / LPMACK and NOTRDY so the "wait for FRZACK" / "wait for
 *     LPMACK" poll completes.
 *   - When firmware leaves Freeze and enables the module (HALT=0, FRZ cleared
 *     or MDIS=0) the model clears NOTRDY / FRZACK / LPMACK so the "wait until
 *     ready" poll completes.
 *   - MCR.SOFTRST self-clears immediately (the reset is instantaneous here).
 *
 * ESR1 (error/status) and IFLAG1 (message-buffer interrupt flags) are
 * write-1-to-clear.  ESR2, CRCR, RXFIR, FDCRC are read-only status that read
 * idle/zero.  All other registers (including the message-buffer RAM, individual
 * mask RAM and enhanced RX FIFO filter RAM) are backed permissively by regs[].
 *
 * Offsets/bits from the MCXN947 CMSIS header (CAN_Type); mode semantics from the
 * FlexCAN chapter of the reference manual.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_flexcan.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Control register offsets (CAN_Type). */
#define R_MCR      0x000   /* Module Configuration */
#define R_CTRL1    0x004
#define R_TIMER    0x008
#define R_ECR      0x01C   /* Error Counter */
#define R_ESR1     0x020   /* Error and Status 1 (W1C flags) */
#define R_IMASK1   0x028
#define R_IFLAG1   0x030   /* Interrupt Flags 1 (W1C) */
#define R_CTRL2    0x034
#define R_ESR2     0x038   /* Error and Status 2 (RO) */
#define R_CRCR     0x044   /* CRC (RO) */
#define R_RXFIR    0x04C   /* Legacy RX FIFO Information (RO) */
#define R_FDCRC    0xC08   /* CAN FD CRC (RO) */

/* MCR bit positions (CAN_MCR_*_SHIFT). */
#define MCR_LPMACK   (1u << 20)
#define MCR_FRZACK   (1u << 24)
#define MCR_SOFTRST  (1u << 25)
#define MCR_NOTRDY   (1u << 27)
#define MCR_HALT     (1u << 28)
#define MCR_FRZ      (1u << 30)
#define MCR_MDIS     (1u << 31)

/* MAXMB (CAN_MCR_MAXMB) occupies bits [6:0]; reset value is 0x0F. */
#define MCR_RESET    (MCR_MDIS | MCR_FRZ | MCR_HALT | MCR_NOTRDY | MCR_FRZACK | \
                      0x0000000Fu)

/*
 * Recompute the MCR acknowledge/ready bits from the freeze/disable request
 * bits.  Freeze is requested by (FRZ and HALT) or by MDIS (low-power).  In
 * either case the module is not ready and acknowledges; otherwise it is ready.
 */
static uint32_t flexcan_mcr_settle(uint32_t mcr)
{
    bool freeze = (mcr & MCR_FRZ) && (mcr & MCR_HALT);
    bool disable = mcr & MCR_MDIS;

    mcr &= ~(MCR_FRZACK | MCR_LPMACK | MCR_NOTRDY);
    if (freeze) {
        mcr |= MCR_FRZACK | MCR_NOTRDY;
    }
    if (disable) {
        mcr |= MCR_LPMACK | MCR_NOTRDY;
    }
    return mcr;
}

static uint64_t flexcan_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNFlexCanState *s = MCXN_FLEXCAN(opaque);
    uint32_t shift = (off & 3) * 8;
    uint32_t idx = off >> 2;
    uint32_t v;

    if (off >= MCXN_FLEXCAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB read @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return 0;
    }

    switch (off & ~3u) {
    case R_ESR2:
    case R_CRCR:
    case R_RXFIR:
    case R_FDCRC:
        /* Read-only status: idle. */
        v = 0;
        break;
    default:
        v = s->regs[idx];
        break;
    }

    /* Support byte/halfword reads by shifting the backing word. */
    return (v >> shift) & ((size == 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1));
}

static void flexcan_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    MCXNFlexCanState *s = MCXN_FLEXCAN(opaque);
    uint32_t idx = off >> 2;
    uint32_t shift = (off & 3) * 8;
    uint32_t mask = (size == 4) ? 0xFFFFFFFFu : (((1u << (size * 8)) - 1) << shift);
    uint32_t val;

    if (off >= MCXN_FLEXCAN_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    /* Merge sub-word writes into the 32-bit register value. */
    val = (s->regs[idx] & ~mask) | ((uint32_t)(value << shift) & mask);

    switch (off & ~3u) {
    case R_MCR:
        /* SOFTRST self-clears: the reset is instantaneous in the model. */
        val &= ~MCR_SOFTRST;
        s->regs[idx] = flexcan_mcr_settle(val);
        return;
    case R_ESR1:
    case R_IFLAG1:
        /* Write-1-to-clear status flags. */
        s->regs[idx] &= ~((uint32_t)(value << shift) & mask);
        return;
    case R_ESR2:
    case R_CRCR:
    case R_RXFIR:
    case R_FDCRC:
        /* Read-only status: ignore writes. */
        return;
    default:
        s->regs[idx] = val;
        return;
    }
}

static const MemoryRegionOps flexcan_ops = {
    .read = flexcan_read,
    .write = flexcan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_flexcan_reset(DeviceState *dev)
{
    MCXNFlexCanState *s = MCXN_FLEXCAN(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_MCR >> 2] = MCR_RESET;
}

static void mcxn_flexcan_realize(DeviceState *dev, Error **errp)
{
    MCXNFlexCanState *s = MCXN_FLEXCAN(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &flexcan_ops, s,
                          TYPE_MCXN_FLEXCAN, MCXN_FLEXCAN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_flexcan = {
    .name = TYPE_MCXN_FLEXCAN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNFlexCanState, MCXN_FLEXCAN_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_flexcan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_flexcan_realize;
    device_class_set_legacy_reset(dc, mcxn_flexcan_reset);
    dc->vmsd = &vmstate_mcxn_flexcan;
}

static const TypeInfo mcxn_flexcan_types[] = {
    {
        .name          = TYPE_MCXN_FLEXCAN,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNFlexCanState),
        .class_init    = mcxn_flexcan_class_init,
    },
};

DEFINE_TYPES(mcxn_flexcan_types)
