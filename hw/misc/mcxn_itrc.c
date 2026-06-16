/*
 * NXP MCX N ITRC (Intrusion and Tamper Response Controller) — faithful
 * register model.
 *
 * Security-violation (SECVIO) and tamper events connect to the ITRC inputs
 * (IN0 to IN47); software maps inputs to outputs (OUT0 to OUT6) via the
 * OUT_SEL selector arrays and can configure an output to assert a chip reset.
 * STATUS and STATUS1 latch which inputs/outputs have fired and are
 * write-1-to-clear.
 *
 * Register map (ITRC_Type):
 *   0x000  STATUS    RW/W1C  IN0 to IN15 + OUT0 to OUT6 event status
 *   0x004  STATUS1   RW/W1C  IN16 to IN47 event status
 *   0x008  OUT_SEL[7][2]     trigger-source selectors  (to 0x03F)
 *   0x048  OUT_SEL_1[7][2]   trigger-source selectors  (to 0x07F)
 *   0x088  OUT_SEL_2[7][2]   trigger-source selectors  (to 0x0BF)
 *   0x0F0  SW_EVENT0 WO      software event 0
 *   0x0F4  SW_EVENT1 WO      software event 1
 *
 * This is a security block.  The model is a register file with NO active
 * tamper response: no input is ever asserted, so STATUS and STATUS1 always
 * read the benign "no event" value (0), software events have no effect, and
 * configuring the ITRC can never reset the machine.  STATUS and STATUS1
 * support write-1-to-clear (a no-op here since no bit is ever set).  Reset
 * value is 0 for all registers.
 *
 * Offsets/access types from the MCXN947 CMSIS header (ITRC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_itrc.h"
#include "migration/vmstate.h"

#define ITRC_STATUS    0x000   /* RW, write-1-to-clear event status        */
#define ITRC_STATUS1   0x004   /* RW, write-1-to-clear event status        */
                               /* 0x008 to 0x0BF: OUT_SEL selector arrays  */
#define ITRC_SW_EVENT0 0x0F0   /* WO software event 0                      */
#define ITRC_SW_EVENT1 0x0F4   /* WO software event 1                      */

static uint64_t mcxn_itrc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNITRCState *s = MCXN_ITRC(opaque);

    switch (offset) {
    case ITRC_STATUS:
    case ITRC_STATUS1:
        /* No tamper/intrusion event is ever generated: read benign value. */
        return 0;
    case ITRC_SW_EVENT0:
    case ITRC_SW_EVENT1:
        /* Write-only software-event registers read 0. */
        return 0;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_itrc_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNITRCState *s = MCXN_ITRC(opaque);

    switch (offset) {
    case ITRC_STATUS:
    case ITRC_STATUS1:
        /*
         * Write-1-to-clear status.  No status bit is ever set in the model,
         * so clearing is a no-op; keep the backing store at 0.
         */
        s->regs[offset / 4] = 0;
        break;
    case ITRC_SW_EVENT0:
    case ITRC_SW_EVENT1:
        /* Software event has no tamper-response side effect in the model. */
        break;
    default:
        /* OUT_SEL selectors and any other config: store faithfully. */
        s->regs[offset / 4] = value;
        break;
    }
}

static const MemoryRegionOps mcxn_itrc_ops = {
    .read = mcxn_itrc_read,
    .write = mcxn_itrc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_itrc_reset(DeviceState *dev)
{
    MCXNITRCState *s = MCXN_ITRC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_itrc_realize(DeviceState *dev, Error **errp)
{
    MCXNITRCState *s = MCXN_ITRC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_itrc_ops, s,
                          TYPE_MCXN_ITRC, MCXN_ITRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_itrc = {
    .name = TYPE_MCXN_ITRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNITRCState, MCXN_ITRC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_itrc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_itrc_realize;
    device_class_set_legacy_reset(dc, mcxn_itrc_reset);
    dc->vmsd = &vmstate_mcxn_itrc;
}

static const TypeInfo mcxn_itrc_types[] = {
    {
        .name          = TYPE_MCXN_ITRC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNITRCState),
        .class_init    = mcxn_itrc_class_init,
    },
};

DEFINE_TYPES(mcxn_itrc_types)
