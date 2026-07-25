/*
 * NXP MCX N EMVSIM (EMV smartcard interface, UART-like) — model.
 *
 * One shared type for EMVSIM0/EMVSIM1.  The transmit path is software-driven
 * and honest: a TX_BUF write transmits the byte synchronously and latches the
 * transmit-complete / early-complete / data-threshold / FIFO-empty flags in
 * TX_STATUS.  When the matching INT_MASK enable bit is clear (per the RM, 0 =
 * interrupt enabled, 1 = masked) this raises the EMVSIM interrupt; the ISR
 * clears the W1C flag to drop the line.  TX_STATUS still reads the transmitter
 * ready for poll-only firmware.  RX_STATUS reports no card data and RX_BUF
 * reads 0.  Bit masks and offsets taken verbatim from the MCXN947 CMSIS header
 * (EMVSIM_Type); IM polarity confirmed against the reference manual.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_emvsim.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* --- Register offsets ------------------------------------------------------ */
#define EMVSIM_VER_ID      0x00  /* RO */
#define EMVSIM_PARAM       0x04  /* RO */
#define EMVSIM_CLKCFG      0x08
#define EMVSIM_DIVISOR     0x0C
#define EMVSIM_CTRL        0x10
#define EMVSIM_INT_MASK    0x14
#define EMVSIM_RX_THD      0x18
#define EMVSIM_TX_THD      0x1C
#define EMVSIM_RX_STATUS   0x20  /* W1C status */
#define EMVSIM_TX_STATUS   0x24  /* W1C status */
#define EMVSIM_PCSR        0x28
#define EMVSIM_RX_BUF      0x2C  /* RO */
#define EMVSIM_TX_BUF      0x30  /* WO */
#define EMVSIM_TX_GETU     0x34
#define EMVSIM_CWT_VAL     0x38
#define EMVSIM_BWT_VAL     0x3C
#define EMVSIM_BGT_VAL     0x40
#define EMVSIM_GPCNT0_VAL  0x44
#define EMVSIM_GPCNT1_VAL  0x48

/* --- TX_STATUS bit masks (CMSIS) ------------------------------------------- */
#define TX_STATUS_TFE   0x00000008u  /* TX FIFO empty */
#define TX_STATUS_ETCF  0x00000010u  /* early transmit complete flag, W1C */
#define TX_STATUS_TCF   0x00000020u  /* transmit complete flag, W1C */
#define TX_STATUS_TFF   0x00000040u  /* TX FIFO full */
#define TX_STATUS_TDTF  0x00000080u  /* TX data transfer flag (threshold), W1C */
#define TX_STATUS_W1C   0x000003FFu  /* TNTE..GPCNT1_TO are all W1C */

/* --- RX_STATUS bit masks (CMSIS) ------------------------------------------- */
#define RX_STATUS_RX_DATA  0x00000010u  /* receiver has data */
#define RX_STATUS_RDTF     0x00000020u  /* rx data threshold, W1C */
#define RX_STATUS_W1C      0x00003FE1u  /* RFO + RDTF..FEF are W1C */

/* --- INT_MASK bit masks (CMSIS); per RM, 0 = enabled, 1 = masked ----------- */
#define INT_MASK_RDT_IM     0x00000001u  /* gates RX_STATUS.RDTF      */
#define INT_MASK_TC_IM      0x00000002u  /* gates TX_STATUS.TCF       */
#define INT_MASK_ETC_IM     0x00000008u  /* gates TX_STATUS.ETCF      */
#define INT_MASK_TFE_IM     0x00000010u  /* gates TX_STATUS.TFE       */
#define INT_MASK_TDT_IM     0x00000080u  /* gates TX_STATUS.TDTF      */
#define INT_MASK_RX_DATA_IM 0x00004000u  /* gates RX_STATUS.RX_DATA   */

/*
 * VER_ID/PARAM are read-only identity registers.  Both used to be invented —
 * the comment here literally read "plausible MCX-class constants", which is the
 * word I use when I mean FABRICATED.  Firmware can size buffers off PARAM, so a
 * made-up depth is a silent-wrong-answer generator, not a cosmetic detail.
 *
 * PARAM: from the RM rev 7 reset value (§69.7.1.3) — bit 12 and bit 4 set, and
 * the fields are TX_FIFO_DEPTH[15:8] / RX_FIFO_DEPTH[7:0], so both depths read
 * 16 bytes.  It was 0x0404 (4/4) here, a number that appears NOWHERE in the RM.
 *
 * ⚠ The RM contradicts ITSELF: the EMVSIM feature list says "transmit FIFO of 8
 * words ... receive FIFO of 8 words", while this register's reset value says 16.
 * We report what the REGISTER says, because that is the value silicon hands
 * firmware and the one a driver would size against — and we disclose the
 * conflict here rather than silently picking a side.  If anyone gets real MCX N
 * silicon, read PARAM and settle it.
 */
#define EMVSIM_PARAM_VALUE   0x00001010u  /* TX depth 16, RX depth 16 */

/*
 * VER_ID: the RM does NOT publish this.  Its reset row is all zeros and the
 * field text offers only "example: 01.00" — so there is no authoritative value
 * to model, and 0x00000100 was pure invention on my part.  We return the
 * documented reset (0) and TELL THE OPERATOR when firmware consumes it, rather
 * than shipping a version number that looks real enough to be gated on.  A
 * stated gap beats a plausible lie.
 */
#define EMVSIM_VER_ID_VALUE  0x00000000u

/*
 * The single EMVSIM interrupt is the OR of the enabled, latched status flags.
 * Only the software-observable transmit events and any latched RX flags can be
 * active in this model (no card -> RX/error sources stay idle).
 */
static void mcxn_emvsim_update_irq(MCXNEMVSIMState *s)
{
    uint32_t mask = s->regs[EMVSIM_INT_MASK / 4];
    uint32_t tx   = s->regs[EMVSIM_TX_STATUS / 4];
    uint32_t rx   = s->regs[EMVSIM_RX_STATUS / 4];
    int level = 0;

    if ((tx & TX_STATUS_TCF)     && !(mask & INT_MASK_TC_IM))      level = 1;
    if ((tx & TX_STATUS_ETCF)    && !(mask & INT_MASK_ETC_IM))     level = 1;
    if ((tx & TX_STATUS_TDTF)    && !(mask & INT_MASK_TDT_IM))     level = 1;
    if ((tx & TX_STATUS_TFE)     && !(mask & INT_MASK_TFE_IM))     level = 1;
    if ((rx & RX_STATUS_RDTF)    && !(mask & INT_MASK_RDT_IM))     level = 1;
    if ((rx & RX_STATUS_RX_DATA) && !(mask & INT_MASK_RX_DATA_IM)) level = 1;

    qemu_set_irq(s->irq, level);
}

static uint64_t mcxn_emvsim_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(opaque);
    uint32_t r;

    switch (offset) {
    case EMVSIM_VER_ID:
        /* Not published by the RM (see above).  If firmware gates on it, the
         * operator needs to know the value it is gating on is not authoritative
         * — we cannot fault the guest through an identity register, but we can
         * refuse to let this pass silently. */
        qemu_log_mask(LOG_UNIMP, "mcxn-emvsim: firmware read VER_ID, which the "
                      "MCX N RM does not publish; returning 0. Any version gate "
                      "on this value is NOT trustworthy.\n");
        return EMVSIM_VER_ID_VALUE;
    case EMVSIM_PARAM:
        return EMVSIM_PARAM_VALUE;
    case EMVSIM_TX_STATUS:
        /*
         * Transmitter is always idle/ready in this model: FIFO empty, not
         * full, transmit complete and threshold met.  Preserve latched W1C
         * flags software set.
         */
        r = s->regs[EMVSIM_TX_STATUS / 4] & TX_STATUS_W1C;
        r &= ~TX_STATUS_TFF;
        r |= TX_STATUS_TFE | TX_STATUS_TCF | TX_STATUS_ETCF | TX_STATUS_TDTF;
        return r;
    case EMVSIM_RX_STATUS:
        /* No card data: report RX empty, keep any latched W1C flags. */
        r = s->regs[EMVSIM_RX_STATUS / 4] & RX_STATUS_W1C;
        r &= ~RX_STATUS_RX_DATA;
        return r;
    case EMVSIM_RX_BUF:
        return 0;  /* RO; no received data */
    case EMVSIM_TX_BUF:
        return 0;  /* write-only */
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_emvsim_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(opaque);

    switch (offset) {
    case EMVSIM_VER_ID:
    case EMVSIM_PARAM:
    case EMVSIM_RX_BUF:
        return;  /* read-only */
    case EMVSIM_TX_STATUS:
        s->regs[EMVSIM_TX_STATUS / 4] &= ~(value & TX_STATUS_W1C);
        mcxn_emvsim_update_irq(s);
        return;
    case EMVSIM_RX_STATUS:
        s->regs[EMVSIM_RX_STATUS / 4] &= ~(value & RX_STATUS_W1C);
        mcxn_emvsim_update_irq(s);
        return;
    case EMVSIM_INT_MASK:
        s->regs[EMVSIM_INT_MASK / 4] = value;
        mcxn_emvsim_update_irq(s);
        return;
    case EMVSIM_TX_BUF:
        /*
         * Synchronous transmit: the byte goes out immediately and the
         * transmit-complete / early-complete / data-threshold / FIFO-empty
         * events latch, driving the interrupt when enabled.
         */
        s->regs[EMVSIM_TX_STATUS / 4] |=
            TX_STATUS_TCF | TX_STATUS_ETCF | TX_STATUS_TDTF | TX_STATUS_TFE;
        mcxn_emvsim_update_irq(s);
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_emvsim_ops = {
    .read = mcxn_emvsim_read,
    .write = mcxn_emvsim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_emvsim_reset(DeviceState *dev)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(dev);

    memset(s->regs, 0, sizeof(s->regs));

    /*
     * RM reset values (register-summary column, §69.7).  memset(0) was wrong for
     * nine registers -- and INT_MASK is FUNCTIONAL, not cosmetic: the IRQ update
     * reads it (0 = enabled, 1 = masked), so 0-at-reset had every EMVSIM interrupt
     * ENABLED out of reset while silicon masks them all -- a status bit set at
     * reset would fire an IRQ the silicon never would.  PCSR[SPDIM] and the wait/
     * threshold/divisor defaults are the values firmware reads before it configures
     * the block; zero is a plausible-but-wrong answer for each.
     */
    s->regs[EMVSIM_DIVISOR / 4]    = 0x00000174u;
    s->regs[EMVSIM_INT_MASK / 4]   = 0x0000FFFFu;   /* all interrupts MASKED (0 = enabled) */
    s->regs[EMVSIM_RX_THD / 4]     = 0x00000001u;
    s->regs[EMVSIM_TX_THD / 4]     = 0x0000000Fu;
    s->regs[EMVSIM_PCSR / 4]       = 0x01000000u;   /* SPDIM: presence-detect IRQ masked */
    s->regs[EMVSIM_CWT_VAL / 4]    = 0x0000FFFFu;
    s->regs[EMVSIM_BWT_VAL / 4]    = 0xFFFFFFFFu;
    s->regs[EMVSIM_GPCNT0_VAL / 4] = 0x0000FFFFu;
    s->regs[EMVSIM_GPCNT1_VAL / 4] = 0x0000FFFFu;
}

static void mcxn_emvsim_realize(DeviceState *dev, Error **errp)
{
    MCXNEMVSIMState *s = MCXN_EMVSIM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_emvsim_ops, s,
                          TYPE_MCXN_EMVSIM, MCXN_EMVSIM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_emvsim = {
    .name = TYPE_MCXN_EMVSIM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNEMVSIMState, MCXN_EMVSIM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_emvsim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_emvsim_realize;
    device_class_set_legacy_reset(dc, mcxn_emvsim_reset);
    dc->vmsd = &vmstate_mcxn_emvsim;
}

static const TypeInfo mcxn_emvsim_types[] = {
    {
        .name          = TYPE_MCXN_EMVSIM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEMVSIMState),
        .class_init    = mcxn_emvsim_class_init,
    },
};

DEFINE_TYPES(mcxn_emvsim_types)
