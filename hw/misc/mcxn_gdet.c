/*
 * NXP MCX N GDET (Digital Glitch Detector) — faithful register model.
 *
 * The MCXN947 has two digital glitch detectors (GDET0, GDET1).  Each exposes a
 * small bank of configuration registers (GDET_CONF_0 to GDET_CONF_5,
 * GDET_ENABLE1) plus reset/test/delay-control registers at the top of the 4 KB
 * window.  All are read-write configuration registers in the CMSIS header
 * (GDET_Type); there are no read-only status or fault-indication registers in
 * the programming model.
 *
 * This is a security block: in hardware a detected voltage glitch can trip a
 * fault that resets the SoC.  The model is purely a register file with NO
 * active glitch/fault side effect, so software can configure and enable the
 * detector without ever tripping a reset.  Registers are backed permissively;
 * reset value is 0 for all (no non-zero default documented in the RM).
 *
 * Offsets/access types from the MCXN947 CMSIS header (GDET_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_gdet.h"
#include "migration/vmstate.h"

#define GDET_CONF_0    0x000   /* RW config */
#define GDET_CONF_1    0x004   /* RW config */
#define GDET_ENABLE1   0x008   /* RW enable */
#define GDET_CONF_2    0x00C   /* RW config */
#define GDET_CONF_3    0x010   /* RW config */
#define GDET_CONF_4    0x014   /* RW config */
#define GDET_CONF_5    0x018   /* RW config */
#define GDET_RESET     0xFC0   /* RW */
#define GDET_TEST      0xFC4   /* RW */
#define GDET_DLY_CTRL  0xFCC   /* RW delay control */

static uint64_t mcxn_gdet_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNGDETState *s = MCXN_GDET(opaque);

    /* All registers are plain read-write configuration; no fault status. */
    return s->regs[offset / 4];
}

static void mcxn_gdet_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNGDETState *s = MCXN_GDET(opaque);

    /*
     * Faithfully store configuration with no behavioural side effect.  In
     * particular, enabling the detector (GDET_ENABLE1) or asserting GDET_RESET
     * must never trip a fault or reset the machine in the model.
     */
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_gdet_ops = {
    .read = mcxn_gdet_read,
    .write = mcxn_gdet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_gdet_reset(DeviceState *dev)
{
    MCXNGDETState *s = MCXN_GDET(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_gdet_realize(DeviceState *dev, Error **errp)
{
    MCXNGDETState *s = MCXN_GDET(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_gdet_ops, s,
                          TYPE_MCXN_GDET, MCXN_GDET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_gdet = {
    .name = TYPE_MCXN_GDET,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNGDETState, MCXN_GDET_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_gdet_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_gdet_realize;
    device_class_set_legacy_reset(dc, mcxn_gdet_reset);
    dc->vmsd = &vmstate_mcxn_gdet;
}

static const TypeInfo mcxn_gdet_types[] = {
    {
        .name          = TYPE_MCXN_GDET,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNGDETState),
        .class_init    = mcxn_gdet_class_init,
    },
};

DEFINE_TYPES(mcxn_gdet_types)
