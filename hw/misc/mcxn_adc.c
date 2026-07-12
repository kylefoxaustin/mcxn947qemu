/*
 * NXP MCX N ADC (LPADC) — bring-up model.  See header.
 *
 * The blocking sequence for firmware is "configure a command, fire a software
 * trigger, poll the result FIFO, read the result".  This model presents a
 * completed conversion the instant a trigger is observed: STAT[RDY0] sets,
 * FCTRL0[FCOUNT] reads one entry, and reading RESFIFO[0] returns the
 * OPERATOR-SET 16-bit result with the VALID bit set.  The sample is NOT invented
 * here: an ADC's answer is whatever voltage is on the pin, so the pin is the
 * seam and the operator drives it (QOM property).  A model that made up a
 * "plausible" reading would be a silent-wrong-answer generator — this comment
 * used to say exactly that, and it was describing the code before the seam was
 * exposed.  CTRL[RST]/CTRL[RSTFIFOn] and the
 * STAT W1C flags self-clear so reset and fifo-flush sequences complete.  All
 * other registers are permissively backed.  Offsets/bits from the MCXN947
 * CMSIS header (ADC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"   /* ctz32 */
#include "hw/misc/mcxn_adc.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
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
#define R_GCC0     0x0F0   /* Gain Calibration Control 0..1 (RO) */
#define R_GCC1     0x0F4
#define R_GCR0     0x0F8   /* Gain Calculation Result 0..1 */
#define R_GCR1     0x0FC
#define GCC_RDY    (1u << 24)   /* CMSIS ADC_GCC_RDY_MASK */

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
#define STAT_CAL_RDY   (1u << 10)  /* CMSIS ADC_STAT_CAL_RDY_MASK */

/* CTRL calibration bits (CMSIS ADC_CTRL_*). */
#define CTRL_CAL_REQ   (1u << 3)   /* auto-calibration request   */
#define CTRL_CALOFS    (1u << 4)   /* offset-calibration enable  */
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

/* Default operator-input value: documented mid-scale (override via QOM). */
#define ADC_CH_DEFAULT 0x0800u

/* CMDL channel + TCTRL command-select fields (CMSIS). */
#define CMDL_ADCH_MASK   0x1Fu
#define TCTRL_TCMD_SHIFT 24
#define TCTRL_TCMD_MASK  0xFu
/* RESFIFO CMDSRC field (which command produced the entry). */
#define RESFIFO_CMDSRC_SHIFT 24
#define RESFIFO_CMDSRC_MASK  0xFu

/* DE (DMA Enable) — the stock LPADC EDMA driver arms these (CMSIS ADC_DE_*). */
#define DE_FWMDE0   0x1u        /* FIFO 0 watermark DMA enable */
#define DE_FWMDE1   0x2u        /* FIFO 1 watermark DMA enable */
#define FCTRL_FCOUNT_MASK  0x1Fu
#define FCTRL_FWMARK_SHIFT 16
#define FCTRL_FWMARK_MASK  0xFu

static void mcxn_adc_update_irq(MCXNADCState *s)
{
    bool active = (s->regs[R_STAT / 4] & s->regs[R_IE / 4] & STAT_W1C_MASK) != 0;
    uint32_t de = s->regs[R_DE / 4];
    int f;

    qemu_set_irq(s->irq, active);

    /*
     * THE DMA REQUEST LINES (ADC0 FIFO A/B = mux sources 21/22, ADC1 = 23/24).
     *
     * ⚠ These did not exist, and DE -- the register the stock LPADC EDMA driver
     * writes to arm them -- was stored and IGNORED.  So the stock
     * driver_examples/lpadc/edma hung at "Configuring LPADC...": it armed an eDMA
     * channel at RESFIFO, set DE[FWMDE0], and waited for a request NOTHING COULD
     * RAISE.
     *
     * The FIFO asks for service when its occupancy EXCEEDS the watermark
     * (FCTRL[FWMARK]), which is exactly the condition the RM gives.
     */
    for (f = 0; f < 2; f++) {
        uint32_t fctrl = s->regs[(f ? R_FCTRL1 : R_FCTRL0) / 4];
        /*
         * ⚠ FCOUNT IS NOT IN regs[].  It is COMPUTED ON READ from s->fifo_valid
         * (see mcxn_adc_read), so reading it back out of the register array gives
         * ZERO, ALWAYS -- and the request would never fire.  The state lives in
         * fifo_valid; the register array is only the shadow.  Read the state.
         */
        uint32_t count = (f == 0 && s->fifo_valid) ? 1 : 0;
        uint32_t wm = (fctrl >> FCTRL_FWMARK_SHIFT) & FCTRL_FWMARK_MASK;
        bool req = (de & (f ? DE_FWMDE1 : DE_FWMDE0)) && count > wm;

        if (req != s->dma_req_level[f]) {
            s->dma_req_level[f] = req;
            qemu_set_irq(s->dma_req[f], req);
        }
    }
}

/*
 * Arm a completed conversion in result FIFO 0.  The result is the OPERATOR-SET
 * value of the channel the triggered command selects (not a hidden constant):
 * SWTRIG bit n -> TCTRLn[TCMD] command index -> CMD[idx].CMDL[ADCH] channel ->
 * adc_ch[channel].  This makes sensor/voltage-dependent guest code read what
 * the operator injects, the way a real board pin would drive it.
 */
static void mcxn_adc_do_conversion(MCXNADCState *s, uint32_t swtrig)
{
    uint32_t trig, cmd = 0, ch = 0;

    if (!(s->regs[R_CTRL / 4] & CTRL_ADCEN)) {
        return;
    }

    /* Lowest set trigger bit selects the trigger; read its starting command. */
    trig = swtrig ? ctz32(swtrig) : 0;
    if (trig < 4) {
        cmd = (s->regs[(R_TCTRL0 + trig * 4) / 4] >> TCTRL_TCMD_SHIFT)
              & TCTRL_TCMD_MASK;
    }
    /* Command index is 1-based; CMDL[ADCH] gives the analog channel. */
    if (cmd >= 1 && cmd <= 15) {
        uint32_t cmdl = s->regs[(R_CMD_BASE + (cmd - 1) * 8) / 4];
        ch = cmdl & CMDL_ADCH_MASK;
    }
    if (ch >= MCXN_ADC_CHANNELS) {
        ch = 0;
    }

    s->fifo_data = (uint32_t)s->adc_ch[ch] | RESFIFO_VALID |
                   ((cmd & RESFIFO_CMDSRC_MASK) << RESFIFO_CMDSRC_SHIFT);
    s->fifo_valid = true;
    s->regs[R_STAT / 4] |= STAT_RDY0;
    mcxn_adc_update_irq(s);
}

static uint64_t mcxn_adc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNADCState *s = MCXN_ADC(opaque);
    uint32_t val;

    switch (offset) {
    case R_GCC0:
    case R_GCC1:
        /*
         * Gain calibration.  LPADC_FinishAutoCalibration() SPINS on GCC[n][RDY]:
         *     while (!(base->GCC[0] & ADC_GCC_RDY_MASK) ||
         *            !(base->GCC[1] & ADC_GCC_RDY_MASK)) { }
         * and these registers were not modelled at all, so it hung forever -- the
         * second half of the SDK's standard ADC init.
         *
         * GAIN_CAL = 0 is the honest value: the driver computes
         * GCR = 131072 / (131072 - GAIN_CAL), which for 0 gives UNITY GAIN.  There
         * is no analog gain error to correct in a model, and inventing a non-zero
         * trim would make the guest apply a correction for a distortion that does
         * not exist.
         */
        return GCC_RDY;

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
    case R_DE:
    case R_FCTRL0:
    case R_FCTRL1:
        /*
         * ⚠ ARMING A DMA-ENABLE MUST RE-EVALUATE THE REQUEST LINE.
         *
         * DE carries FWMDE0/FWMDE1 -- the bits the stock LPADC EDMA driver sets to
         * ask the eDMA for service -- and FCTRL carries the watermark.  These fell
         * through to a PLAIN STORE, so a driver that armed DE last (as they all do)
         * never raised a request at all.
         *
         * ⭐ THIS IS THE THIRD TIME TODAY: the DAC's DER, the LPUART's BAUD[TDMAE],
         * and now the ADC's DE.  Identical bug, three peripherals.  A FIX APPLIED IN
         * ONE PLACE IS NOT A FIX -- I wrote that sentence this morning and then made
         * the same mistake twice more.  Any register that gates a request line must
         * call the update function on write.  Check that FIRST when adding one.
         */
        s->regs[offset / 4] = v;
        mcxn_adc_update_irq(s);
        return;

    case R_CTRL:
        /*
         * ⚠ CALIBRATION MUST COMPLETE, OR THE STOCK DRIVER HANGS ON ITS FIRST STEP.
         *
         * LPADC_DoOffsetCalibration() sets CTRL[CALOFS] and then spins:
         *     while (!(base->STAT & ADC_STAT_CAL_RDY_MASK)) { }
         * and LPADC_DoAutoCalibration() does the same with CTRL[CAL_REQ].  This is
         * what the SDK's STANDARD ADC INIT does, so ANY firmware using the stock
         * LPADC driver hung here forever -- STAT[CAL_RDY] was never set.
         *
         * My hand-written ADC test passed the whole time, because I never called
         * calibration.  The stock driver hangs on its very first step.  (Found by
         * running the real driver and then asking gdb WHERE THE CPU WAS, rather
         * than interrogating the subsystem I suspected.)
         *
         * There is no analog trim to model, so calibration completes immediately.
         */
        if (v & (CTRL_CALOFS | CTRL_CAL_REQ)) {
            s->regs[R_STAT / 4] |= STAT_CAL_RDY;
        }

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
            mcxn_adc_do_conversion(s, v);
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
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    s->fifo_data = 0;
    s->fifo_valid = false;
    /* Default analog inputs to documented mid-scale (operator overrides persist
     * across guest soft-resets — this only re-defaults on a full machine reset). */
    for (i = 0; i < MCXN_ADC_CHANNELS; i++) {
        s->adc_ch[i] = ADC_CH_DEFAULT;
    }
    qemu_set_irq(s->irq, 0);
}

static void mcxn_adc_init(Object *obj)
{
    MCXNADCState *s = MCXN_ADC(obj);
    int i;

    /* Expose each analog input as a runtime QOM property "adc-chN" so an
     * operator can inject the value a board pin would drive:
     *   qom-set /machine/.../adc0 adc-ch5 2748   */
    for (i = 0; i < MCXN_ADC_CHANNELS; i++) {
        g_autofree char *name = g_strdup_printf("adc-ch%d", i);
        s->adc_ch[i] = ADC_CH_DEFAULT;
        object_property_add_uint16_ptr(obj, name, &s->adc_ch[i],
                                       OBJ_PROP_FLAG_READWRITE);
    }
}

static void mcxn_adc_realize(DeviceState *dev, Error **errp)
{
    MCXNADCState *s = MCXN_ADC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_adc_ops, s,
                          TYPE_MCXN_ADC, MCXN_ADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req[0]);  /* -> eDMA 21/23 */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req[1]);  /* -> eDMA 22/24 */
}

static const VMStateDescription vmstate_mcxn_adc = {
    .name = TYPE_MCXN_ADC,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNADCState, MCXN_ADC_SIZE / 4),
        VMSTATE_UINT16_ARRAY(adc_ch, MCXNADCState, MCXN_ADC_CHANNELS),
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
        .instance_init = mcxn_adc_init,
        .class_init    = mcxn_adc_class_init,
    },
};

DEFINE_TYPES(mcxn_adc_types)
