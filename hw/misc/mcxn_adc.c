/*
 * NXP MCX N ADC (LPADC) — bring-up model.  See header.
 *
 * The blocking sequence for firmware is "configure a command, fire a software
 * trigger, poll the result FIFO, read the result".  This model presents a
 * completed conversion the instant a trigger is observed: STAT[RDY0] sets,
 * FCTRL0[FCOUNT] reads one entry, and reading RESFIFO[0] returns a plausible
 * 16-bit result with the VALID bit set.  CTRL[RST]/CTRL[RSTFIFOn] and the
 * STAT W1C flags self-clear so reset and fifo-flush sequences complete.  All
 * other registers are permissively backed.  Offsets/bits from the MCXN947
 * CMSIS header (ADC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_adc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS ADC_Type). */
#define R_VERID    0x000   /* RO */
#define R_PARAM    0x004   /* RO */
#define R_CTRL     0x010
#define R_STAT     0x014
#define R_IE       0x018
#define R_DE       0x01C
#define R_CFG      0x020
#define R_PAUSE    0x024
#define R_SWTRIG   0x034   /* WO */
#define R_TSTAT    0x038
#define R_OFSTRIM  0x040
#define R_TCTRL0   0x0A0   /* TCTRL0..3 @0xA0..0xAC */
#define R_FCTRL0   0x0E0   /* FCTRL0..1 @0xE0..0xE4 */
#define R_FCTRL1   0x0E4
#define R_GCC0     0x0F0   /* GCC0..1   @0xF0..0xF4 (RO) */
#define R_GCR0     0x0F8   /* GCR0..1   @0xF8..0xFC */
#define R_CMD_BASE 0x100   /* CMD[15], step 0x8: CMDL/CMDH */
#define R_CMD_END  0x178
#define R_CV0      0x200   /* CV0..14   @0x200.. */
#define R_CV_END   0x238
#define R_RESFIFO0 0x300   /* RESFIFO0..1 @0x300..0x304 (RO) */
#define R_RESFIFO1 0x304
#define R_CAL_GAR0 0x400   /* CAL_GAR[33] */
#define R_CAL_GAR_END 0x480
#define R_CAL_GBR0 0x500   /* CAL_GBR[33] */
#define R_CAL_GBR_END 0x580

/* CTRL bits. */
#define CTRL_ADCEN     (1u << 0)
#define CTRL_RST       (1u << 1)
#define CTRL_RSTFIFO0  (1u << 8)
#define CTRL_RSTFIFO1  (1u << 9)

/* STAT bits (write-1-to-clear flags). */
#define STAT_RDY0      (1u << 0)
#define STAT_FOF0      (1u << 1)
#define STAT_RDY1      (1u << 2)
#define STAT_FOF1      (1u << 3)
#define STAT_TEXC_INT  (1u << 8)
#define STAT_TCOMP_INT (1u << 9)
#define STAT_CAL_RDY   (1u << 10)
#define STAT_ADC_ACTIVE (1u << 11)
#define STAT_W1C_MASK  (STAT_RDY0 | STAT_FOF0 | STAT_RDY1 | STAT_FOF1 | \
                        STAT_TEXC_INT | STAT_TCOMP_INT)

/* IE bits. */
#define IE_FWMIE0      (1u << 0)

/* FCTRL[FCOUNT] field. */
#define FCTRL_FCOUNT_SHIFT 0
#define FCTRL_FCOUNT_MASK  0x1Fu

/* RESFIFO bits. */
#define RESFIFO_D_MASK 0xFFFFu
#define RESFIFO_VALID  (1u << 31)

/*
 * VERID/PARAM read-only constants.  Exact RM table values could not be
 * confirmed by text extraction; these match the LPADC instantiation on
 * MCXN947 (major.minor 2.0, 2 FIFOs, 15 commands, 15 compare values, single
 * sec/64-bit FIFO entries, differential supported).  Marked best-effort.
 */
#define ADC_VERID_VALUE   0x02000209u   /* MAJOR=2 MINOR=0 NUM_FIFO=2 ... */
#define ADC_PARAM_VALUE   0x0F0F0F08u   /* CMD_NUM=15 CV_NUM=15 FIFOSIZE=15 TRIG_NUM=8 */

/* A plausible mid-scale 16-bit conversion result. */
#define ADC_RESULT_SAMPLE 0x0800u

static void mcxn_adc_update_irq(MCXNADCState *s)
{
    bool active = (s->regs[R_STAT / 4] & s->regs[R_IE / 4] & STAT_W1C_MASK) != 0;
    qemu_set_irq(s->irq, active);
}

/* Arm a completed conversion in result FIFO 0. */
static void mcxn_adc_do_conversion(MCXNADCState *s)
{
    if (!(s->regs[R_CTRL / 4] & CTRL_ADCEN)) {
        return;
    }
    s->fifo_data = ADC_RESULT_SAMPLE | RESFIFO_VALID;
    s->fifo_valid = true;
    s->regs[R_STAT / 4] |= STAT_RDY0;
    mcxn_adc_update_irq(s);
}

static uint64_t mcxn_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNADCState *s = MCXN_ADC(opaque);
    uint32_t val;

    switch (offset) {
    case R_VERID:
        return ADC_VERID_VALUE;
    case R_PARAM:
        return ADC_PARAM_VALUE;
    case R_SWTRIG:
        return 0;   /* write-only */
    case R_FCTRL0:
        val = s->regs[R_FCTRL0 / 4] & ~FCTRL_FCOUNT_MASK;
        if (s->fifo_valid) {
            val |= (1u << FCTRL_FCOUNT_SHIFT) & FCTRL_FCOUNT_MASK;
        }
        return val;
    case R_FCTRL1:
        return s->regs[R_FCTRL1 / 4] & ~FCTRL_FCOUNT_MASK;
    case R_RESFIFO0:
        if (s->fifo_valid) {
            s->fifo_valid = false;
            s->regs[R_STAT / 4] &= ~STAT_RDY0;
            mcxn_adc_update_irq(s);
            return s->fifo_data;
        }
        return 0;   /* empty FIFO: VALID bit clear */
    case R_RESFIFO1:
        return 0;   /* FIFO 1 unused in this model */
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_adc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNADCState *s = MCXN_ADC(opaque);
    uint32_t v = value;

    switch (offset) {
    case R_VERID:
    case R_PARAM:
        return;   /* read-only */
    case R_CTRL:
        /* RST and RSTFIFOn are self-clearing soft resets. */
        if (v & CTRL_RST) {
            s->regs[R_STAT / 4] = 0;
            s->fifo_valid = false;
        }
        if (v & (CTRL_RSTFIFO0 | CTRL_RSTFIFO1)) {
            s->fifo_valid = false;
            s->regs[R_STAT / 4] &= ~(STAT_RDY0 | STAT_RDY1 |
                                     STAT_FOF0 | STAT_FOF1);
        }
        s->regs[R_CTRL / 4] = v & ~(CTRL_RST | CTRL_RSTFIFO0 | CTRL_RSTFIFO1);
        mcxn_adc_update_irq(s);
        return;
    case R_STAT:
        /* Write-1-to-clear the flag bits; preserve the rest. */
        s->regs[R_STAT / 4] &= ~(v & STAT_W1C_MASK);
        if (v & STAT_RDY0) {
            s->fifo_valid = false;
        }
        mcxn_adc_update_irq(s);
        return;
    case R_IE:
        s->regs[R_IE / 4] = v;
        mcxn_adc_update_irq(s);
        return;
    case R_SWTRIG:
        /* Any software trigger arms a completed conversion. */
        if (v) {
            mcxn_adc_do_conversion(s);
        }
        return;
    case R_TCTRL0:
    case R_TCTRL0 + 4:
    case R_TCTRL0 + 8:
    case R_TCTRL0 + 12:
        s->regs[offset / 4] = v;
        return;
    case R_RESFIFO0:
    case R_RESFIFO1:
        return;   /* read-only */
    default:
        s->regs[offset / 4] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_adc_ops = {
    .read = mcxn_adc_read,
    .write = mcxn_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_adc_reset(DeviceState *dev)
{
    MCXNADCState *s = MCXN_ADC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->fifo_data = 0;
    s->fifo_valid = false;
    qemu_set_irq(s->irq, 0);
}

static void mcxn_adc_realize(DeviceState *dev, Error **errp)
{
    MCXNADCState *s = MCXN_ADC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_adc_ops, s,
                          TYPE_MCXN_ADC, MCXN_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_adc = {
    .name = TYPE_MCXN_ADC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNADCState, MCXN_ADC_SIZE / 4),
        VMSTATE_UINT32(fifo_data, MCXNADCState),
        VMSTATE_BOOL(fifo_valid, MCXNADCState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_adc_realize;
    device_class_set_legacy_reset(dc, mcxn_adc_reset);
    dc->vmsd = &vmstate_mcxn_adc;
}

static const TypeInfo mcxn_adc_types[] = {
    {
        .name          = TYPE_MCXN_ADC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNADCState),
        .class_init    = mcxn_adc_class_init,
    },
};

DEFINE_TYPES(mcxn_adc_types)
