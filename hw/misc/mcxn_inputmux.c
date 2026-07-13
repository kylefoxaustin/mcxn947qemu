/*
 * NXP MCX N INPUTMUX (input multiplexer / trigger routing) — bring-up model.
 *
 * Permissive register-backed model.  Every INPUTMUX register is a routing
 * selector (__IO) or a write-only SET/CLR/TOG alias (__O); all reset to 0 and
 * have no externally-observable behavior in emulation, so storing and reading
 * back the written value is faithful.  Offsets from the MCXN947 CMSIS header
 * (INPUTMUX_Type, base 0x40006000, registers up to offset 0x7B8).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_inputmux.h"
#include "hw/core/irq.h"
#include "hw/core/qdev.h"
#include "migration/vmstate.h"

/*
 * DMAn_REQ_ENABLE{0..3}: one bit per eDMA request source, with the usual NXP
 * SET/CLR/TOG write aliases.  RM 26.5.1.56: "DMA request 0-31 enable for DMA0.  One
 * bit per request.  0: DMA request to DMA0 and response from DMA0 are blocked.
 * 1: DMA request and response are enabled for DMA0."
 *
 *   DMA0_REQ_ENABLE0 @0x700  (SET 0x704, CLR 0x708, TOG 0x70C)
 *   DMA0_REQ_ENABLE1 @0x710 ... DMA0_REQ_ENABLE3 @0x730
 *   DMA1_REQ_ENABLE0 @0x780 ... DMA1_REQ_ENABLE3 @0x7B0
 *
 * Reset (RM): FFFF_FFFF, FFFF_FFFF, FFFF_FFFF, 03FF_FFFF -- ALL 122 REQUEST LINES
 * ENABLED OUT OF RESET.  That reset value is why nothing broke while this was
 * unmodelled: the gate is open by default, so every stock example still worked.  It
 * is also exactly why it was worth modelling -- a guest that CLOSES a gate expects
 * the request to stop, and in an ungated model it does not.  The model was MORE
 * PERMISSIVE THAN THE SILICON, which ships the bug to the board.
 */
#define IM_DMA0_REQ_ENABLE0  0x700
#define IM_DMA1_REQ_ENABLE0  0x780
#define IM_REQ_ENABLE_STRIDE 0x010   /* per 32-bit bank: reg, SET, CLR, TOG */
#define IM_REQ_ENABLE_BANKS  4

static const uint32_t im_req_enable_reset[IM_REQ_ENABLE_BANKS] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x03FFFFFFu
};

/* Map an offset to (dma, bank, alias) or return false. */
static bool im_decode_req_enable(hwaddr off, int *dma, int *bank, int *alias)
{
    hwaddr rel;

    if (off >= IM_DMA0_REQ_ENABLE0 &&
        off < IM_DMA0_REQ_ENABLE0 + IM_REQ_ENABLE_BANKS * IM_REQ_ENABLE_STRIDE) {
        *dma = 0;
        rel = off - IM_DMA0_REQ_ENABLE0;
    } else if (off >= IM_DMA1_REQ_ENABLE0 &&
               off < IM_DMA1_REQ_ENABLE0 + IM_REQ_ENABLE_BANKS * IM_REQ_ENABLE_STRIDE) {
        *dma = 1;
        rel = off - IM_DMA1_REQ_ENABLE0;
    } else {
        return false;
    }
    *bank  = rel / IM_REQ_ENABLE_STRIDE;
    *alias = (rel % IM_REQ_ENABLE_STRIDE) / 4;   /* 0=reg 1=SET 2=CLR 3=TOG */
    return true;
}

/* Push a bank's 32 gate levels out to the eDMA. */
static void im_push_req_enable(MCXNInputMuxState *s, int dma, int bank)
{
    uint32_t base = (dma ? IM_DMA1_REQ_ENABLE0 : IM_DMA0_REQ_ENABLE0)
                    + bank * IM_REQ_ENABLE_STRIDE;
    uint32_t v = s->regs[base / 4];
    int i;

    for (i = 0; i < 32; i++) {
        int src = bank * 32 + i;

        if (src < MCXN_INPUTMUX_NREQ) {
            qemu_set_irq(s->dma_req_enable[dma][src], (v >> i) & 1);
        }
    }
}

static uint64_t mcxn_inputmux_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(opaque);

    return s->regs[offset / 4];
}

static void mcxn_inputmux_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(opaque);
    int dma, bank, alias;

    if (im_decode_req_enable(offset, &dma, &bank, &alias)) {
        uint32_t base = (dma ? IM_DMA1_REQ_ENABLE0 : IM_DMA0_REQ_ENABLE0)
                        + bank * IM_REQ_ENABLE_STRIDE;
        uint32_t *reg = &s->regs[base / 4];

        switch (alias) {
        case 0: *reg  = value; break;   /* the register itself */
        case 1: *reg |= value; break;   /* SET */
        case 2: *reg &= ~(uint32_t)value; break;   /* CLR */
        case 3: *reg ^= value; break;   /* TOG */
        default: break;
        }
        im_push_req_enable(s, dma, bank);
        return;
    }

    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_inputmux_ops = {
    .read = mcxn_inputmux_read,
    .write = mcxn_inputmux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_inputmux_reset(DeviceState *dev)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(dev);
    int dma, bank;

    memset(s->regs, 0, sizeof(s->regs));

    /* RM: every eDMA request line is ENABLED out of reset.  A zero here would
     * block every peripheral-triggered transfer on the chip. */
    for (dma = 0; dma < MCXN_INPUTMUX_NDMA; dma++) {
        uint32_t base = dma ? IM_DMA1_REQ_ENABLE0 : IM_DMA0_REQ_ENABLE0;

        for (bank = 0; bank < IM_REQ_ENABLE_BANKS; bank++) {
            s->regs[(base + bank * IM_REQ_ENABLE_STRIDE) / 4] =
                im_req_enable_reset[bank];
        }
    }
}

static void mcxn_inputmux_realize(DeviceState *dev, Error **errp)
{
    MCXNInputMuxState *s = MCXN_INPUTMUX(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_inputmux_ops, s,
                          TYPE_MCXN_INPUTMUX, MCXN_INPUTMUX_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /* One gate line per eDMA request source, per eDMA (DMA0 then DMA1). */
    qdev_init_gpio_out_named(dev, s->dma_req_enable[0], "dma0-req-enable",
                             MCXN_INPUTMUX_NREQ);
    qdev_init_gpio_out_named(dev, s->dma_req_enable[1], "dma1-req-enable",
                             MCXN_INPUTMUX_NREQ);
}

static const VMStateDescription vmstate_mcxn_inputmux = {
    .name = TYPE_MCXN_INPUTMUX,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNInputMuxState, MCXN_INPUTMUX_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_inputmux_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_inputmux_realize;
    device_class_set_legacy_reset(dc, mcxn_inputmux_reset);
    dc->vmsd = &vmstate_mcxn_inputmux;
}

static const TypeInfo mcxn_inputmux_types[] = {
    {
        .name          = TYPE_MCXN_INPUTMUX,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNInputMuxState),
        .class_init    = mcxn_inputmux_class_init,
    },
};

DEFINE_TYPES(mcxn_inputmux_types)
