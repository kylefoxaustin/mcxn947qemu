/*
 * NXP MCX N SPC (System Power Controller) — bring-up model.
 *
 * Firmware programs SRAM/core/system voltage and polls for completion.  The key
 * blocker for bring-up is SRAMCTL's REQ->ACK handshake: software sets REQ
 * (bit 30) and spins until hardware sets ACK (bit 31).  Here ACK simply tracks
 * REQ (instant completion).  SC.BUSY reads idle.  All other registers are
 * permissively backed.  Offsets/bits from the MCXN947 CMSIS header (SPC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_spc.h"
#include "migration/vmstate.h"

#define SPC_VERID    0x00   /* RO */
#define SPC_SC       0x10   /* Status Control */
#define SPC_SRAMCTL  0x40   /* SRAM Control */

#define SPC_SC_BUSY        (1u << 0)
#define SPC_SRAMCTL_REQ    (1u << 30)
#define SPC_SRAMCTL_ACK    (1u << 31)

#define SPC_VERID_VALUE    0x00000001u

static uint64_t mcxn_spc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSPCState *s = MCXN_SPC(opaque);
    uint32_t val;

    switch (offset) {
    case SPC_VERID:
        return SPC_VERID_VALUE;
    case SPC_SC:
        /* Power transitions are instantaneous in the model: never busy. */
        return s->regs[SPC_SC / 4] & ~SPC_SC_BUSY;
    case SPC_SRAMCTL:
        /* Acknowledge instantly: ACK mirrors REQ. */
        val = s->regs[SPC_SRAMCTL / 4];
        if (val & SPC_SRAMCTL_REQ) {
            val |= SPC_SRAMCTL_ACK;
        } else {
            val &= ~SPC_SRAMCTL_ACK;
        }
        return val;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_spc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNSPCState *s = MCXN_SPC(opaque);

    if (offset == SPC_VERID) {
        return;  /* read-only */
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps mcxn_spc_ops = {
    .read = mcxn_spc_read,
    .write = mcxn_spc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * ⚠ SPC RESET VALUES -- FROM THE RM, VIA
 *   tests/mcxn-reset-values/extract-rm-golden.py.  DERIVED, NEVER INVENTED.
 *   Cross-checked against the CMSIS field masks.
 *
 * This block used to be `memset(regs, 0)` and nothing else -- 13 registers
 * answering zero where the manual says otherwise, and ALL 13 were sitting in
 * the allowlist, excused by a "reason" that was just a restatement of the
 * mismatch.
 *
 * ☠ SPC IS THE SYSTEM POWER CONTROLLER, AND ZERO IS A LEGAL, MEANINGFUL,
 *   CATASTROPHIC VALUE IN IT.  This is 91emulator's dangerous-zero rule
 *   landing on the one block whose entire job is keeping the chip alive:
 *
 *   CNTRL = 0x7 = CORELDO_EN | SYSLDO_EN | DCDC_EN.  ALL THREE REGULATORS ARE
 *   ON AT RESET -- of course they are, the chip is RUNNING.  We answered 0:
 *   all three OFF.  And fsl_spc.c read-modify-writes this register:
 *
 *       base->CNTRL |= SPC_CNTRL_CORELDO_EN_MASK;
 *
 *   ⭐ SO A GUEST ENABLING *ONE* REGULATOR READ OUR ZERO, OR'd IN ITS BIT,
 *      WROTE BACK -- AND SILENTLY SWITCHED OFF THE SYS LDO AND THE DC-DC
 *      CONVERTER IT NEVER TOUCHED.  On silicon that is a brownout.
 *      (93emulator's RMW-laundering class, on the power rails: the wrong reset
 *      value is laundered into the guest's OWN state, and nothing in the model
 *      ever reads it back to notice.)
 *
 *   VD_CORE_CFG / VD_SYS_CFG = 0x1 = LVDRE: Low-Voltage-Detect RESET Enable.
 *   We answered 0 -- BROWNOUT RESET DISABLED.  A guest that RMW'd this to arm
 *   the LVD *interrupt* would silently disarm the RESET that stops the core
 *   executing garbage off a collapsing supply.  The safety mechanism, switched
 *   off by reading a lie.
 *
 *   ACTIVE_VDELAY = 0xC8 (200): the core-voltage settle delay.  Zero says the
 *   rail settles INSTANTLY.
 */
static const struct { uint16_t off; uint32_t val; } spc_reset[] = {
    { 0x014, 0x00000007u },  /* CNTRL            CORELDO_EN|SYSLDO_EN|DCDC_EN */
    { 0x040, 0x00000001u },  /* SRAMCTL                                       */
    { 0x100, 0x3F100615u },  /* ACTIVE_CFG                                    */
    { 0x104, 0x00000002u },  /* ACTIVE_CFG1                                   */
    { 0x108, 0x00021504u },  /* LP_CFG                                        */
    { 0x10C, 0x00000002u },  /* LP_CFG1                                       */
    { 0x124, 0x000000C8u },  /* ACTIVE_VDELAY    200: rail settle delay       */
    { 0x134, 0x00000001u },  /* VD_CORE_CFG      LVDRE: brownout reset ARMED  */
    { 0x138, 0x00000001u },  /* VD_SYS_CFG       LVDRE                        */
    { 0x13C, 0x00000101u },  /* VD_IO_CFG        LVDRE | LOCK                 */
    { 0x144, 0x0000003Fu },  /* GLITCH_DETECT_SC                              */
    { 0x400, 0x00000101u },  /* SYSLDO_CFG                                    */
    { 0x504, 0x01400000u },  /* DCDC_BURST_CFG                                */
};

static void mcxn_spc_reset(DeviceState *dev)
{
    MCXNSPCState *s = MCXN_SPC(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < ARRAY_SIZE(spc_reset); i++) {
        s->regs[spc_reset[i].off / 4] = spc_reset[i].val;
    }
}

static void mcxn_spc_realize(DeviceState *dev, Error **errp)
{
    MCXNSPCState *s = MCXN_SPC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_spc_ops, s,
                          TYPE_MCXN_SPC, MCXN_SPC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_spc = {
    .name = TYPE_MCXN_SPC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSPCState, MCXN_SPC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_spc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_spc_realize;
    device_class_set_legacy_reset(dc, mcxn_spc_reset);
    dc->vmsd = &vmstate_mcxn_spc;
}

static const TypeInfo mcxn_spc_types[] = {
    {
        .name          = TYPE_MCXN_SPC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSPCState),
        .class_init    = mcxn_spc_class_init,
    },
};

DEFINE_TYPES(mcxn_spc_types)
