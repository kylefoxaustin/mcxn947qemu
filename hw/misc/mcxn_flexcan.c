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
#include "qapi/error.h"
#include "hw/misc/mcxn_flexcan.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
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
#define R_RXMGMASK 0x010   /* RX Message Buffers Global Mask */

/* CTRL1 loopback-mode enable. */
#define CTRL1_LPB  (1u << 12)

/*
 * Classic message-buffer array: MB[32] at offset 0x80, step 0x10
 * (CS, ID, WORD0, WORD1).  CS holds CODE[27:24] and DLC[19:16].
 */
#define MB_BASE        0x080
#define MB_STRIDE      0x010
#define MB_COUNT       32
#define MB_CODE_SHIFT  24
#define MB_CODE_MASK   0xFu
#define CODE_RX_EMPTY  0x4   /* MB configured to receive, currently empty */
#define CODE_RX_FULL   0x2   /* MB holds a received frame */
#define CODE_RX_OVERRUN 0x6  /* 0110b: a frame was overwritten into a full MB
                              * (RM rev 7, message-buffer CODE table).  This is
                              * how the silicon TELLS the guest it lost one —
                              * the model used to just drop the frame in silence */
#define CODE_TX_DATA   0xC   /* MB armed to transmit a data frame */

/* CS control bits (classic frame): SRR[22], IDE[21], RTR[20], DLC[19:16]. */
#define MB_SRR    (1u << 22)
#define MB_IDE    (1u << 21)
#define MB_RTR    (1u << 20)
#define MB_DLC(cs)  (((cs) >> 16) & 0xF)

/* MCR bit positions (CAN_MCR_*_SHIFT). */
#define MCR_LPMACK   (1u << 20)
#define MCR_SUPV     (1u << 23)   /* supervisor access; reset = 1 */
#define MCR_FRZACK   (1u << 24)
#define MCR_SOFTRST  (1u << 25)
#define MCR_NOTRDY   (1u << 27)
#define MCR_HALT     (1u << 28)
#define MCR_FRZ      (1u << 30)
#define MCR_MDIS     (1u << 31)

/* MAXMB (CAN_MCR_MAXMB) occupies bits [6:0]; reset value is 0x0F.
 * Reset = 0xD890_000F (RM): MDIS|FRZ|HALT|NOTRDY | SUPV | LPMACK | MAXMB=0xF.  At reset the
 * module is DISABLED (MDIS=1), so it acknowledges LOW-POWER (LPMACK), NOT freeze (FRZACK) --
 * FRZACK requires the module ENABLED.  The old constant set FRZACK and dropped SUPV/LPMACK
 * (0xD900_000F), overriding the correct value in the reset table below. */
#define MCR_RESET    (MCR_MDIS | MCR_FRZ | MCR_HALT | MCR_NOTRDY | MCR_SUPV | MCR_LPMACK | \
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
    if (disable) {
        mcr |= MCR_LPMACK | MCR_NOTRDY;   /* low-power takes precedence: a disabled */
    } else if (freeze) {                   /* module acks LOW-POWER, never FREEZE   */
        mcr |= MCR_FRZACK | MCR_NOTRDY;
    }
    return mcr;
}

static void flexcan_update_irq(MCXNFlexCanState *s)
{
    bool active = (s->regs[R_IFLAG1 >> 2] & s->regs[R_IMASK1 >> 2]) != 0;
    qemu_set_irq(s->irq, active);
}

/*
 * Software transmit from message buffer "tx".  When the controller is in
 * loopback mode (CTRL1.LPB), the transmitted frame is delivered internally to
 * the first RX-empty message buffer whose ID matches under the global mask
 * (RXMGMASK; a 0 mask bit is "don't care", so a reset mask of 0 accepts any
 * ID).  The receiving MB is filled (CODE=FULL, ID/DLC/data copied) and its
 * IFLAG1 bit set; the transmitting MB also raises its IFLAG1 (transmit done).
 */
/* Build a classic qemu_can_frame from TX message buffer "tx" and put it on the
 * emulated CAN bus (a can-host-chardev bridges the bus to a socket peer). */
static void flexcan_send_to_bus(MCXNFlexCanState *s, unsigned tx)
{
    uint32_t t = (MB_BASE + tx * MB_STRIDE) >> 2;
    uint32_t cs = s->regs[t], id = s->regs[t + 1];
    qemu_can_frame f = { 0 };
    uint8_t len = MB_DLC(cs), i;

    if (len > 8) {
        len = 8;
    }
    if (cs & MB_IDE) {
        f.can_id = (id & QEMU_CAN_EFF_MASK) | QEMU_CAN_EFF_FLAG;
    } else {
        f.can_id = (id >> 18) & QEMU_CAN_SFF_MASK;
    }
    if (cs & MB_RTR) {
        f.can_id |= QEMU_CAN_RTR_FLAG;
    }
    f.can_dlc = len;
    for (i = 0; i < len; i++) {
        f.data[i] = (s->regs[t + 2 + i / 4] >> (24 - 8 * (i % 4))) & 0xFF;
    }
    can_bus_client_send(&s->bus_client, &f, 1);
}

/* Is this controller on the bus at all right now?  A DISABLED or FROZEN
 * FlexCAN neither receives NOR transmits. */
static bool flexcan_enabled(MCXNFlexCanState *s)
{
    uint32_t mcr = s->regs[R_MCR >> 2];

    /* A DISABLED or FROZEN FlexCAN is not on the bus at all.  This used to
     * return true unconditionally, so a controller the guest had switched off
     * still quietly filled its mailboxes with traffic the silicon would never
     * have delivered. */
    return !(mcr & MCR_MDIS) && !(mcr & MCR_NOTRDY);
}

/*
 * A frame arrived from the CAN bus.  Run the RM's matching process (rev 7,
 * "Matching process"): scan the receive message buffers and deliver the frame
 * to the first one whose ID matches under the mask.
 *
 * Two silent-wrong-answer bugs used to live here, and both are the kind a CAN
 * node in a board farm would never be able to diagnose from inside firmware:
 *
 *  1. NO ID MATCHING AT ALL.  The frame was dropped into the first CODE=EMPTY
 *     mailbox, whatever ID that mailbox had been configured for.  A driver that
 *     trusts its own filter -- "MB3 is my ID, so whatever lands in MB3 is mine"
 *     -- would read somebody else's frame and never know.  The old comment
 *     defended this with "RXMGMASK=0 => any ID; the guest filters in software",
 *     which is only true at RESET: the moment a driver programs mailbox IDs and
 *     a mask, the model ignored both.
 *
 *  2. A FULL MAILBOX MEANT A SILENTLY DROPPED FRAME.  The old code scanned only
 *     for CODE=EMPTY and, finding none, returned success having thrown the frame
 *     away -- the comment even admitted "a real device flags overrun".  Per the
 *     RM the matching process considers MBs whose CODE is EMPTY, FULL *or*
 *     OVERRUN, and when a new frame lands on an unserviced buffer the buffer is
 *     OVERWRITTEN and CODE becomes OVERRUN (0110b).  So the data still moves and
 *     the guest is TOLD it lost one.  Dropping in silence is the worst of both.
 */
static ssize_t flexcan_bus_receive(CanBusClientState *client,
                                   const qemu_can_frame *frames,
                                   size_t frames_cnt)
{
    MCXNFlexCanState *s = container_of(client, MCXNFlexCanState, bus_client);
    const qemu_can_frame *f = frames;
    uint32_t mask = s->regs[R_RXMGMASK >> 2];
    uint32_t rx_id;
    bool eff, rtr;
    unsigned rx;
    uint8_t len, i;

    if (!frames_cnt || (f->can_id & QEMU_CAN_ERR_FLAG)) {
        return frames_cnt;
    }
    if (!flexcan_enabled(s)) {
        return frames_cnt;         /* module disabled/frozen: not on the bus */
    }

    eff = f->can_id & QEMU_CAN_EFF_FLAG;
    rtr = f->can_id & QEMU_CAN_RTR_FLAG;
    len = f->can_dlc > 8 ? 8 : f->can_dlc;

    /* MB ID registers hold standard IDs left-aligned at bit 18. */
    rx_id = eff ? (f->can_id & QEMU_CAN_EFF_MASK)
                : ((f->can_id & QEMU_CAN_SFF_MASK) << 18);

    for (rx = 0; rx < MB_COUNT; rx++) {
        uint32_t r = (MB_BASE + rx * MB_STRIDE) >> 2;
        uint32_t code = (s->regs[r] >> MB_CODE_SHIFT) & MB_CODE_MASK;
        bool full;

        /* The matching process considers EMPTY, FULL and OVERRUN buffers. */
        if (code != CODE_RX_EMPTY && code != CODE_RX_FULL &&
            code != CODE_RX_OVERRUN) {
            continue;
        }
        /* A 0 mask bit is "don't care", so the reset mask of 0 accepts any ID
         * — which is what made the missing filter look correct for so long. */
        if (((rx_id ^ s->regs[r + 1]) & mask) != 0) {
            continue;              /* this mailbox is not listening for this ID */
        }

        full = (code != CODE_RX_EMPTY);   /* unserviced: this is an overrun */

        s->regs[r + 1] = rx_id;
        s->regs[r + 2] = s->regs[r + 3] = 0;
        for (i = 0; i < len; i++) {
            s->regs[r + 2 + i / 4] |= (uint32_t)f->data[i] << (24 - 8 * (i % 4));
        }
        s->regs[r] = ((full ? CODE_RX_OVERRUN : CODE_RX_FULL) << MB_CODE_SHIFT) |
                     ((uint32_t)len << 16) |
                     (eff ? (MB_IDE | MB_SRR) : 0) | (rtr ? MB_RTR : 0);
        s->regs[R_IFLAG1 >> 2] |= (1u << rx);
        flexcan_update_irq(s);
        return 1;
    }

    /* Nothing was listening for this ID.  That is not an error: on a real bus
     * every node sees every frame and ignores the ones it did not filter for. */
    return 1;
}

static bool flexcan_bus_can_receive(CanBusClientState *client)
{
    MCXNFlexCanState *s = container_of(client, MCXNFlexCanState, bus_client);

    return flexcan_enabled(s);
}

static CanBusClientInfo flexcan_bus_client_info = {
    .can_receive = flexcan_bus_can_receive,
    .receive     = flexcan_bus_receive,
};

static void flexcan_transmit(MCXNFlexCanState *s, unsigned tx)
{
    uint32_t t = (MB_BASE + tx * MB_STRIDE) >> 2;
    uint32_t tcs = s->regs[t];
    uint32_t tid = s->regs[t + 1];

    /* A disabled or frozen module is not on the bus: it does not transmit, and
     * (in loopback) it does not deliver to itself either.  Gating only the
     * receive path left a switched-off controller still looping frames back into
     * its own mailboxes. */
    if (!flexcan_enabled(s)) {
        return;
    }

    if (s->canbus && !(s->regs[R_CTRL1 >> 2] & CTRL1_LPB)) {
        /* Board-to-board: put the frame on the real CAN bus. */
        flexcan_send_to_bus(s, tx);
    } else if (s->regs[R_CTRL1 >> 2] & CTRL1_LPB) {
        uint32_t mask = s->regs[R_RXMGMASK >> 2];
        unsigned rx;

        for (rx = 0; rx < MB_COUNT; rx++) {
            uint32_t r = (MB_BASE + rx * MB_STRIDE) >> 2;
            uint32_t rcs = s->regs[r];

            if (rx == tx) {
                continue;
            }
            uint32_t rcode = (rcs >> MB_CODE_SHIFT) & MB_CODE_MASK;
            bool full;

            /* Matching considers EMPTY, FULL and OVERRUN buffers (RM rev 7).
             * Only scanning for EMPTY meant a frame arriving on an unserviced
             * mailbox was DROPPED IN SILENCE. */
            if (rcode != CODE_RX_EMPTY && rcode != CODE_RX_FULL &&
                rcode != CODE_RX_OVERRUN) {
                continue;
            }
            if (((tid ^ s->regs[r + 1]) & mask) != 0) {
                continue;
            }
            full = (rcode != CODE_RX_EMPTY);   /* unserviced => overrun */

            /* Deliver the frame: keep DLC/RTR/IDE/SRR; CODE=FULL, or OVERRUN if
             * we just overwrote a buffer the CPU had not read yet. */
            s->regs[r]     = ((full ? CODE_RX_OVERRUN : CODE_RX_FULL)
                              << MB_CODE_SHIFT) | (tcs & 0x00FF0000u);
            s->regs[r + 1] = tid;
            s->regs[r + 2] = s->regs[t + 2];
            s->regs[r + 3] = s->regs[t + 3];
            s->regs[R_IFLAG1 >> 2] |= (1u << rx);
            break;
        }
    }

    /* Transmit complete: the TX message buffer raises its own interrupt. */
    s->regs[R_IFLAG1 >> 2] |= (1u << tx);
    flexcan_update_irq(s);
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
        /* Write-1-to-clear status flags. */
        s->regs[idx] &= ~((uint32_t)(value << shift) & mask);
        return;
    case R_IFLAG1:
        /* Write-1-to-clear MB interrupt flags; re-evaluate the IRQ. */
        s->regs[idx] &= ~((uint32_t)(value << shift) & mask);
        flexcan_update_irq(s);
        return;
    case R_IMASK1:
        s->regs[idx] = val;
        flexcan_update_irq(s);
        return;
    case R_ESR2:
    case R_CRCR:
    case R_RXFIR:
    case R_FDCRC:
        /* Read-only status: ignore writes. */
        return;
    default:
        s->regs[idx] = val;
        /* Arming a message buffer for transmit (CS CODE=TX_DATA) sends it. */
        if (off >= MB_BASE && off < MB_BASE + MB_COUNT * MB_STRIDE &&
            ((off - MB_BASE) % MB_STRIDE) == 0 &&
            ((val >> MB_CODE_SHIFT) & MB_CODE_MASK) == CODE_TX_DATA) {
            flexcan_transmit(s, (off - MB_BASE) / MB_STRIDE);
        }
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

/*
 * CAN0 reset values, from the RM's register map (generated by
 * tests/mcxn-reset-values/extract-rm-golden.py -- derived, never invented).
 */
static const struct { uint16_t off; uint32_t val; } rst_can0[] = {
    { 0x000, 0xD890000Fu },   /* MCR */
    { 0x034, 0x00A00000u },   /* CTRL2 */
    { 0xB00, 0x00000100u },   /* CTRL1_PN */
    { 0xB10, 0x00000008u },   /* FLT_DLC */
    { 0xC00, 0x80000100u },   /* FDCTRL */
};

static void mcxn_flexcan_reset(DeviceState *dev)
{
    MCXNFlexCanState *s = MCXN_FLEXCAN(dev);
    int rst_i;

    memset(s->regs, 0, sizeof(s->regs));
    for (rst_i = 0; rst_i < (int)ARRAY_SIZE(rst_can0); rst_i++) {
        s->regs[rst_can0[rst_i].off / 4] = rst_can0[rst_i].val;
    }
    s->regs[R_MCR >> 2] = MCR_RESET;
}

static void mcxn_flexcan_realize(DeviceState *dev, Error **errp)
{
    MCXNFlexCanState *s = MCXN_FLEXCAN(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &flexcan_ops, s,
                          TYPE_MCXN_FLEXCAN, MCXN_FLEXCAN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /* Board-to-board CAN: join the emulated bus if one was linked. */
    if (s->canbus) {
        s->bus_client.info = &flexcan_bus_client_info;
        if (can_bus_insert_client(s->canbus, &s->bus_client) < 0) {
            error_setg(errp, "failed to join CAN bus");
            return;
        }
    }
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

static const Property mcxn_flexcan_properties[] = {
    DEFINE_PROP_LINK("canbus", MCXNFlexCanState, canbus, TYPE_CAN_BUS,
                     CanBusState *),
};

static void mcxn_flexcan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_flexcan_realize;
    device_class_set_legacy_reset(dc, mcxn_flexcan_reset);
    dc->vmsd = &vmstate_mcxn_flexcan;
    device_class_set_props(dc, mcxn_flexcan_properties);
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
