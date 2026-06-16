/*
 * NXP MCX N RTC (Real-Time Clock / calendar) — bring-up model.  See header.
 *
 * Firmware unlocks the block (STATUS[WE]), takes it out of software reset
 * (CTRL[SWR], self-clearing), programs the calendar counters and polls status.
 * This model presents the oscillator/clock as running: the calendar counters
 * read back whatever software wrote (no live tick is required for bring-up),
 * the write-enable field is modelled permissively, and the subsecond and wake
 * timer enable bits read back as written.  ISR flags are write-1-to-clear.
 * Offsets/bits from the MCXN947 CMSIS header (RTC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_rtc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS RTC_Type).  16-bit registers except the 32-bit
 * subsecond and wake-timer registers above 0x800. */
#define R_YEARMON      0x00   /* 16-bit */
#define R_DAYS         0x02   /* 16-bit */
#define R_HOURMIN      0x04   /* 16-bit */
#define R_SECONDS      0x06   /* 16-bit */
#define R_ALM_YEARMON  0x08
#define R_ALM_DAYS     0x0A
#define R_ALM_HOURMIN  0x0C
#define R_ALM_SECONDS  0x0E
#define R_CTRL         0x10   /* 16-bit */
#define R_STATUS       0x12   /* 16-bit */
#define R_ISR          0x14   /* 16-bit, W1C flags */
#define R_IER          0x16   /* 16-bit */
#define R_RTC_TEST2    0x1C   /* RO sub-second counter */
#define R_DST_HOUR     0x22
#define R_DST_MONTH    0x24
#define R_DST_DAY      0x26
#define R_COMPEN       0x28
#define R_SUBSECOND_CTRL 0x800
#define R_SUBSECOND_CNT  0x804   /* RO */
#define R_WAKE_TIMER_CTRL 0xC00
#define R_WAKE_TIMER_CNT  0xC0C

/* CTRL bits. */
#define CTRL_SWR        (1u << 8)   /* software reset, self-clearing */

/* STATUS bits. */
#define STATUS_WE_SHIFT 6
#define STATUS_WE_MASK  (0x3u << STATUS_WE_SHIFT)   /* write enable field */
#define STATUS_CMP_INT  (1u << 5)

/* ISR flags (write-1-to-clear). */
#define ISR_W1C_MASK    0xFFFCu     /* ALM/DAY/HOUR/MIN/sample-rate flags */

/* SUBSECOND_CTRL. */
#define SUBSECOND_CNT_EN (1u << 0)

/* WAKE_TIMER_CTRL. */
#define WAKE_TIMER_WAKE_FLAG (1u << 1)
#define WAKE_TIMER_INTR_EN   (1u << 5)

static bool mcxn_rtc_is_16bit(hwaddr offset)
{
    return offset < 0x800;
}

static void mcxn_rtc_update_irq(MCXNRTCState *s)
{
    /* ISR (0x14) and IER (0x16) are two 16-bit registers sharing one 32-bit
     * backing word: ISR is the low half, IER the high half. */
    uint32_t word = s->regs[R_ISR / 4];
    uint16_t isr = word & 0xFFFF;
    uint16_t ier = (word >> 16) & 0xFFFF;

    qemu_set_irq(s->irq, (isr & ier) != 0);
}

static uint64_t mcxn_rtc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNRTCState *s = MCXN_RTC(opaque);
    uint32_t word = s->regs[offset / 4];

    /* Two adjacent 16-bit registers share one 32-bit backing word.  Return
     * the correct half for a 16-bit access; a 32-bit access returns the
     * whole word. */
    if (mcxn_rtc_is_16bit(offset) && size <= 2) {
        if (offset & 2) {
            return (word >> 16) & 0xFFFF;
        }
        return word & 0xFFFF;
    }
    return word;
}

static void mcxn_rtc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNRTCState *s = MCXN_RTC(opaque);
    uint32_t v = value;

    /* 16-bit register region: merge the written half into the backing word. */
    if (mcxn_rtc_is_16bit(offset)) {
        hwaddr reg = offset & ~1;   /* 16-bit aligned register address */
        uint32_t word = s->regs[reg / 4];
        uint16_t half = v & 0xFFFF;

        switch (reg) {
        case R_CTRL:
            /* SWR is a self-clearing software reset. */
            if (half & CTRL_SWR) {
                half &= ~CTRL_SWR;
            }
            break;
        case R_STATUS:
            /* WE (write-enable) is modelled permissively: keep what software
             * sets so the lock/unlock dance reads back as expected. */
            break;
        case R_ISR:
            /* Write-1-to-clear flag bits. */
            half = (s->regs[reg / 4] & 0xFFFF) & ~(half & ISR_W1C_MASK);
            break;
        default:
            break;
        }

        if (reg & 2) {
            word = (word & 0x0000FFFF) | ((uint32_t)half << 16);
        } else {
            word = (word & 0xFFFF0000) | half;
        }
        s->regs[reg / 4] = word;
        mcxn_rtc_update_irq(s);
        return;
    }

    /* 32-bit register region. */
    switch (offset) {
    case R_SUBSECOND_CNT:
    case R_WAKE_TIMER_CNT:
        return;   /* read-only counters */
    case R_WAKE_TIMER_CTRL:
        /* WAKE_FLAG is write-1-to-clear; other control bits read back. */
        if (v & WAKE_TIMER_WAKE_FLAG) {
            s->regs[offset / 4] &= ~WAKE_TIMER_WAKE_FLAG;
            v &= ~WAKE_TIMER_WAKE_FLAG;
        }
        s->regs[offset / 4] = (s->regs[offset / 4] & WAKE_TIMER_WAKE_FLAG) | v;
        return;
    default:
        s->regs[offset / 4] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_rtc_ops = {
    .read = mcxn_rtc_read,
    .write = mcxn_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_rtc_reset(DeviceState *dev)
{
    MCXNRTCState *s = MCXN_RTC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void mcxn_rtc_realize(DeviceState *dev, Error **errp)
{
    MCXNRTCState *s = MCXN_RTC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_rtc_ops, s,
                          TYPE_MCXN_RTC, MCXN_RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_rtc = {
    .name = TYPE_MCXN_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNRTCState, MCXN_RTC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_rtc_realize;
    device_class_set_legacy_reset(dc, mcxn_rtc_reset);
    dc->vmsd = &vmstate_mcxn_rtc;
}

static const TypeInfo mcxn_rtc_types[] = {
    {
        .name          = TYPE_MCXN_RTC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNRTCState),
        .class_init    = mcxn_rtc_class_init,
    },
};

DEFINE_TYPES(mcxn_rtc_types)
