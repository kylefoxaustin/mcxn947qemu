/*
 * NXP MCX N PORT (pin mux / pin control) — bring-up stub.
 *
 * Permissive register-backed model: PCR[] and the other PORT registers are
 * stored and read back so firmware pin-mux/pull configuration completes.  Pin
 * muxing has no effect on the emulated GPIO/peripheral function.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_port.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define PORT_VERID  0x00   /* RO */

#define PORT_VERID_VALUE 0x01000000u

static uint64_t mcxn_port_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNPortState *s = MCXN_PORT(opaque);

    if (offset == PORT_VERID) {
        return PORT_VERID_VALUE;
    }
    return s->regs[offset / 4];
}

static void mcxn_port_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNPortState *s = MCXN_PORT(opaque);

    if (offset == PORT_VERID) {
        return;  /* read-only */
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_port_ops = {
    .read = mcxn_port_read,
    .write = mcxn_port_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * The NXP SDK PORT driver writes PCR as a 16-bit halfword
     * (PORT_SetPinConfig does `*(volatile uint16_t*)&base->PCR[pin] = ...`).
     * Accept byte/halfword accesses from the guest; keep the handler
     * word-only (impl=4) so QEMU adapts sub-word writes into a word
     * read-modify-write over regs[].
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * PAD RESET VALUES -- AND THEY ARE PER-PORT, WHICH IS WHY THE RM DEFERS THEM.
 *
 * PCR's row in the register-summary table says "See section" in the reset
 * column, and the section explains why:
 *
 *     Register reset values
 *     Register    Reset value
 *     PCR0        PORT0:        0000_1143h
 *                 PORT1-PORT5:  0000_0000h
 *
 * ⭐ THE RESET VALUE DEPENDS ON THE INSTANCE.  Our reset-value gate
 * stores ONE reset per (name, offset) and SHARES IT ACROSS INSTANCES,
 * so it CANNOT REPRESENT THIS -- and the extractor, correctly, refused
 * the row rather than pick one.  A missing register is a gap; a wrong
 * one is a false witness.  So this is hand-read from the RM, and named
 * in known-deviations.txt as a structural limit of the golden rather
 * than a bug in it.
 *
 * WHAT IT COSTS.  These four pads are non-zero out of reset -- PORT0's
 * PCR0/PCR3/PCR6 carry MUX=1 with pulls enabled, i.e. THE SWD DEBUG
 * PINS -- and we reset them to ZERO.
 * The SDK's PORT_SetPinConfig does a READ-MODIFY-WRITE on the pad:
 *
 *     *(volatile uint16_t *)&base->PCR[pin] = ...
 *
 * so firmware that touches a neighbouring field reads a value THE
 * SILICON WOULD NEVER PRODUCE and writes back a pad configuration that
 * never existed -- silently clobbering the debug pin's mux and pull.
 * (91emulator found the identical class on their pinmux: "any driver
 * doing a read-modify-write on a pad reads a value the silicon would
 * never produce.")
 */
static const struct {
    uint8_t port; uint8_t pcr; uint32_t val;
} port_pad_reset[] = {
    { 0,  0, 0x00001143u },   /* RM 75.6.1.11: PORT0 PCR0  (SWD) */
    { 0,  3, 0x00001103u },   /* RM: PORT0 PCR3            (SWD) */
    { 0,  6, 0x00001103u },   /* RM: PORT0 PCR6            (SWD) */
    { 5,  2, 0x00000100u },   /* RM: PORT5 PCR2                  */
};

#define PORT_PCR0_OFFSET 0x80

static void mcxn_port_reset(DeviceState *dev)
{
    MCXNPortState *s = MCXN_PORT(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));

    for (i = 0; i < (int)ARRAY_SIZE(port_pad_reset); i++) {
        if (port_pad_reset[i].port == s->port_id) {
            uint32_t off = PORT_PCR0_OFFSET + 4u * port_pad_reset[i].pcr;

            s->regs[off / 4] = port_pad_reset[i].val;
        }
    }
}

static const Property mcxn_port_properties[] = {
    DEFINE_PROP_UINT8("port-id", MCXNPortState, port_id, 0),
};

static void mcxn_port_realize(DeviceState *dev, Error **errp)
{
    MCXNPortState *s = MCXN_PORT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_port_ops, s,
                          TYPE_MCXN_PORT, MCXN_PORT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_port = {
    .name = TYPE_MCXN_PORT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNPortState, MCXN_PORT_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_port_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, mcxn_port_properties);

    dc->realize = mcxn_port_realize;
    device_class_set_legacy_reset(dc, mcxn_port_reset);
    dc->vmsd = &vmstate_mcxn_port;
}

static const TypeInfo mcxn_port_types[] = {
    {
        .name          = TYPE_MCXN_PORT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNPortState),
        .class_init    = mcxn_port_class_init,
    },
};

DEFINE_TYPES(mcxn_port_types)
