/*
 * NXP MCX N TSI (Touch Sensing Input) — bring-up model.  See header.
 *
 * Firmware enables the block (GENCS[TSIEN]), starts a scan (software trigger
 * GENCS[SWTS], or periodic via GENCS[STM]) and polls the end-of-scan flag
 * DATA[EOSF] before reading the conversion counter DATA[TSICNT].  This model
 * completes a scan the instant a software trigger is observed: DATA[EOSF] sets
 * and DATA[TSICNT] reads the OPERATOR-SET count for the selected channel — the
 * value a real electrode's capacitance would drive.  The count is NOT invented
 * here; the electrode is the seam and the operator drives it (QOM property).  EOSF and the overrun/out-of-range
 * flags are write-1-to-clear, so the scan-complete handshake finishes.  All
 * other registers are permissively backed.  Offsets/bits from the MCXN947
 * CMSIS header (TSI_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_tsi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS TSI_Type). */
#define R_CONFIG    0x00   /* CONFIG / CONFIG_MUTUAL (union) */
#define R_TSHD      0x04
#define R_GENCS     0x08
#define R_MUL       0x0C
#define R_SINC      0x10
#define R_SSC0      0x14
#define R_SSC1      0x18
#define R_SSC2      0x1C
#define R_BASELINE  0x20
#define R_CHMERGE   0x24
#define R_SHIELD    0x28
#define R_DATA      0x100
#define R_MISC      0x108
#define R_TRIG      0x10C

/* CONFIG bits (channel select). */
#define CONFIG_TSICH_SHIFT 1
#define CONFIG_TSICH_MASK  0x3Eu     /* bits [5:1]: scanned channel index */

/* GENCS bits. */
#define GENCS_STM       (1u << 3)    /* scan trigger mode (periodic) */
#define GENCS_TSIEN     (1u << 5)    /* module enable */
#define GENCS_SWTS      (1u << 7)    /* software trigger, self-clearing */

/* DATA bits (the flags are write-1-to-clear). */
#define DATA_TSICNT_MASK 0xFFFFu
#define DATA_EOSF        (1u << 27)  /* end-of-scan flag */
#define DATA_OVERRUNF    (1u << 29)
#define DATA_OUTRGF      (1u << 30)
#define DATA_W1C_MASK    (DATA_EOSF | DATA_OVERRUNF | DATA_OUTRGF)

/* Default operator-input value: a documented sample count (override via QOM). */
#define TSI_COUNT_SAMPLE 0x0100u

static void mcxn_tsi_update_irq(MCXNTSIState *s)
{
    /* Assert while the module is enabled and the end-of-scan flag is set. */
    bool active = (s->regs[R_GENCS / 4] & GENCS_TSIEN) &&
                  (s->regs[R_DATA / 4] & DATA_EOSF);
    qemu_set_irq(s->irq, active);
}

/*
 * Complete a scan: latch the OPERATOR-SET count of the selected channel and
 * raise the end-of-scan flag.  The channel comes from CONFIG[TSICH]; the count
 * is tsi_count[channel] (the value a real electrode's capacitance would drive,
 * injectable via the "tsi-countN" QOM property) instead of a hidden constant.
 */
static void mcxn_tsi_do_scan(MCXNTSIState *s)
{
    uint32_t ch;

    if (!(s->regs[R_GENCS / 4] & GENCS_TSIEN)) {
        return;
    }
    ch = (s->regs[R_CONFIG / 4] & CONFIG_TSICH_MASK) >> CONFIG_TSICH_SHIFT;
    if (ch >= MCXN_TSI_CHANNELS) {
        ch = 0;
    }
    s->regs[R_DATA / 4] = (s->regs[R_DATA / 4] & ~DATA_TSICNT_MASK) |
                          ((uint32_t)s->tsi_count[ch] & DATA_TSICNT_MASK) |
                          DATA_EOSF;
    mcxn_tsi_update_irq(s);
}

/*
 * A HARDWARE trigger routed in by INPUTMUX from TSI_TRIG (an LPTMR compare): start a scan,
 * the counterpart of the ADC's "convert on a timer tick" for touch sensing -- a low-power
 * loop wakes the TSI periodically off a timer with no CPU involvement.  GENCS[STM] gates it:
 * the routed trigger scans only in HARDWARE-trigger mode (STM=1); in software mode (STM=0)
 * the block is driven by SWTS instead and a stray routed edge must NOT scan.
 */
static void mcxn_tsi_hw_trigger(void *opaque, int n, int level)
{
    MCXNTSIState *s = MCXN_TSI(opaque);

    if (!level) {
        return;                          /* a trigger is an edge, not a level */
    }
    if (!(s->regs[R_GENCS / 4] & GENCS_STM)) {
        return;                          /* software-trigger mode: ignore the routed trigger */
    }
    mcxn_tsi_do_scan(s);                 /* do_scan re-checks GENCS[TSIEN] */
}

static uint64_t mcxn_tsi_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNTSIState *s = MCXN_TSI(opaque);

    switch (offset) {
    case R_GENCS:
        /* SWTS reads back 0 (the trigger is momentary). */
        return s->regs[R_GENCS / 4] & ~GENCS_SWTS;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_tsi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNTSIState *s = MCXN_TSI(opaque);
    uint32_t v = value;

    switch (offset) {
    case R_GENCS:
        /* SWTS is a self-clearing software trigger.  GENCS[STM] only SELECTS the trigger
         * source (0 = software / SWTS, 1 = hardware, i.e. the INPUTMUX-routed TSI_TRIG);
         * it does NOT itself start a scan -- a scan in hardware-trigger mode waits for the
         * routed edge (mcxn_tsi_hw_trigger).  The old code scanned on the STM write, which
         * FABRICATED one scan with no trigger source behind it. */
        s->regs[R_GENCS / 4] = v & ~GENCS_SWTS;
        if (v & GENCS_SWTS) {
            mcxn_tsi_do_scan(s);
        } else {
            mcxn_tsi_update_irq(s);
        }
        return;
    case R_DATA: {
        /* Write-1-to-clear the flag bits; the count field is read-only. */
        uint32_t cur = s->regs[R_DATA / 4];
        cur &= ~(v & DATA_W1C_MASK);
        s->regs[R_DATA / 4] = cur;
        mcxn_tsi_update_irq(s);
        return;
    }
    default:
        s->regs[offset / 4] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_tsi_ops = {
    .read = mcxn_tsi_read,
    .write = mcxn_tsi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * ⚠ TSI CAME UP AS memset(0).  RM reset values, derived -- never invented.
 *
 * ☠ SINC[DECIMATION] (bits 20:16) RESETS TO 7.  Ours was ZERO -- A DECIMATION FACTOR OF
 *   ZERO, which is a divide-by-zero in the CIC filter that consumes it.  That is a
 *   dangerous zero in the exact sense this tree has been chasing all week: legal,
 *   meaningful, and catastrophic to anything that divides by it.
 *
 * The SSC0/1/2 prescalers and thresholds and the GENCS/SHIELD configuration are likewise
 * read-modify-written by the stock touch driver, so our zeros were being laundered into
 * the guest's own configuration (93emulator's class).
 */
static const struct { uint16_t off; uint32_t val; } tsi_reset[] = {
    { 0x008, 0x02001000u },   /* GENCS                                   */
    { 0x010, 0x00070001u },   /* SINC      DECIMATION = 7, not zero      */
    { 0x014, 0x60320000u },   /* SSC0                                    */
    { 0x018, 0x00600040u },   /* SSC1                                    */
    { 0x01C, 0x10080101u },   /* SSC2                                    */
    { 0x020, 0x00010000u },   /* BASELINE                                */
    { 0x028, 0x04000000u },   /* SHIELD                                  */
};

static void mcxn_tsi_reset(DeviceState *dev)
{
    MCXNTSIState *s = MCXN_TSI(dev);
    int i;

    int ti;

    memset(s->regs, 0, sizeof(s->regs));
    for (ti = 0; ti < (int)ARRAY_SIZE(tsi_reset); ti++) {
        s->regs[tsi_reset[ti].off / 4] = tsi_reset[ti].val;
    }
    /* Default electrodes to the documented sample count (operator overrides
     * persist across guest soft-resets; this re-defaults on machine reset). */
    for (i = 0; i < MCXN_TSI_CHANNELS; i++) {
        s->tsi_count[i] = TSI_COUNT_SAMPLE;
    }
    qemu_set_irq(s->irq, 0);
}

static void mcxn_tsi_init(Object *obj)
{
    MCXNTSIState *s = MCXN_TSI(obj);
    int i;

    /* Expose each electrode's scan counter as a runtime QOM property
     * "tsi-countN" so an operator can inject the value a real touch would
     * drive:  qom-set /machine/.../tsi0 tsi-count3 1840   */
    for (i = 0; i < MCXN_TSI_CHANNELS; i++) {
        g_autofree char *name = g_strdup_printf("tsi-count%d", i);
        s->tsi_count[i] = TSI_COUNT_SAMPLE;
        object_property_add_uint16_ptr(obj, name, &s->tsi_count[i],
                                       OBJ_PROP_FLAG_READWRITE);
    }
}

static void mcxn_tsi_realize(DeviceState *dev, Error **errp)
{
    MCXNTSIState *s = MCXN_TSI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_tsi_ops, s,
                          TYPE_MCXN_TSI, MCXN_TSI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    /* Hardware trigger routed in by INPUTMUX from TSI_TRIG (LPTMR -> periodic scan). */
    qdev_init_gpio_in_named(dev, mcxn_tsi_hw_trigger, "trigger", 1);
}

static const VMStateDescription vmstate_mcxn_tsi = {
    .name = TYPE_MCXN_TSI,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNTSIState, MCXN_TSI_SIZE / 4),
        VMSTATE_UINT16_ARRAY(tsi_count, MCXNTSIState, MCXN_TSI_CHANNELS),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_tsi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_tsi_realize;
    device_class_set_legacy_reset(dc, mcxn_tsi_reset);
    dc->vmsd = &vmstate_mcxn_tsi;
}

static const TypeInfo mcxn_tsi_types[] = {
    {
        .name          = TYPE_MCXN_TSI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNTSIState),
        .instance_init = mcxn_tsi_init,
        .class_init    = mcxn_tsi_class_init,
    },
};

DEFINE_TYPES(mcxn_tsi_types)
