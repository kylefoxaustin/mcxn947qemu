/*
 * NXP MCX N TDET (Digital Tamper) — faithful register model.
 *
 * The Digital Tamper module (CMSIS type DIGTMP) supports active and passive
 * tamper-pin functions.  When a pin tamper event is detected it can generate a
 * chip reset.  Software configures the control/lock/enable registers and reads
 * tamper status from SR.
 *
 * Register map (DIGTMP_Type), all 32-bit read-write:
 *   0x10  CR    Control      (SWR, DEN, TFSR, UM, ATCS0/1, DISTAM, DPR)
 *   0x14  SR    Status       (DTF, TAF, TIF0 to TIF9, TPF0 to TPF7) W1C
 *   0x18  LR    Lock         (CRL, SRL)
 *   0x1C  IER   Interrupt Enable
 *   0x20  TSR   Tamper Seconds
 *   0x24  TER   Tamper Enable
 *   0x28  PDR   Pin Direction
 *   0x2C  PPR   Pin Polarity
 *   0x30  ATR[2]   Active Tamper (0x30, 0x34)
 *   0x40  PGFR[8]  Pin Glitch Filter (0x40 to 0x5C)
 *
 * This is a security block.  The model is a register file with NO active
 * tamper response: no tamper pin is ever asserted, so the SR status register
 * always reads the benign "no tamper" value (0), and configuring/enabling the
 * TDET can never reset the machine.  SR is write-1-to-clear (a no-op here
 * since no flag is ever set).  Reset value is 0 for all registers.
 *
 * Offsets/access types from the MCXN947 CMSIS header (DIGTMP_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_tdet.h"
#include "migration/vmstate.h"

#define TDET_CR    0x10   /* Control                              */
#define TDET_SR    0x14   /* Status, write-1-to-clear             */
#define TDET_LR    0x18   /* Lock                                 */
#define TDET_IER   0x1C   /* Interrupt Enable                     */
#define TDET_TSR   0x20   /* Tamper Seconds                       */
#define TDET_TER   0x24   /* Tamper Enable                        */
#define TDET_PDR   0x28   /* Pin Direction                        */
#define TDET_PPR   0x2C   /* Pin Polarity                         */
/* 0x30 to 0x37: ATR[2] active tamper; 0x40 to 0x5F: PGFR[8] glitch filter */

static uint64_t mcxn_tdet_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNTDETState *s = MCXN_TDET(opaque);

    switch (offset) {
    case TDET_SR:
        /* No tamper event is ever generated: read benign value. */
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_tdet_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNTDETState *s = MCXN_TDET(opaque);

    switch (offset) {
    case TDET_SR:
        /*
         * Write-1-to-clear status.  No flag is ever set in the model, so
         * clearing is a no-op; keep the backing store at 0.
         */
        s->regs[offset / 4] = 0;
        break;
    default:
        /*
         * Store control/lock/enable/config faithfully with no behavioural
         * side effect.  Enabling tamper detection must never assert a tamper
         * response or reset the machine in the model.
         */
        s->regs[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps mcxn_tdet_ops = {
    .read = mcxn_tdet_read,
    .write = mcxn_tdet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_tdet_reset(DeviceState *dev)
{
    MCXNTDETState *s = MCXN_TDET(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_tdet_realize(DeviceState *dev, Error **errp)
{
    MCXNTDETState *s = MCXN_TDET(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_tdet_ops, s,
                          TYPE_MCXN_TDET, MCXN_TDET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_tdet = {
    .name = TYPE_MCXN_TDET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNTDETState, MCXN_TDET_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_tdet_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_tdet_realize;
    device_class_set_legacy_reset(dc, mcxn_tdet_reset);
    dc->vmsd = &vmstate_mcxn_tdet;
}

static const TypeInfo mcxn_tdet_types[] = {
    {
        .name          = TYPE_MCXN_TDET,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNTDETState),
        .class_init    = mcxn_tdet_class_init,
    },
};

DEFINE_TYPES(mcxn_tdet_types)
