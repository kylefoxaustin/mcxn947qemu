/*
 * NXP MCX N CMP (Low-Power Analog Comparator, CMSIS LPCMP_Type) — model.
 *
 * One shared device type instantiated three times on the MCXN947 (CMP0/1/2).
 * The comparator is a pure analog block; QEMU does not simulate the analog
 * inputs, so a faithful register model (correct offsets, access types, reset
 * values and the write-1-to-clear status semantics) is the correct behaviour.
 *
 *   - VERID / PARAM are read-only (CMSIS __I) and return constant values.
 *   - CSR is the Comparator Status register.  COUT (bit 8) is the comparator
 *     output level: with no analog stimulus it reads 0.  CFR (bit 0, rising
 *     edge), CFF (bit 1, falling edge) and RRF (bit 2, round-robin) are
 *     write-1-to-clear sticky flags; with no stimulus they stay 0.
 *   - The comparator has no multi-cycle power-up handshake that firmware spins
 *     on, so all control registers are simply stored.
 *
 * Offsets/bits/access-types from the MCXN947 CMSIS header (LPCMP_Type); reset
 * values from the MCX N Reference Manual (section 44.7.1, LPCMP memory map).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_cmp.h"
#include "migration/vmstate.h"

#define CMP_VERID    0x00   /* RO  Version ID */
#define CMP_PARAM    0x04   /* RO  Parameter */
#define CMP_CCR0     0x08   /* RW  Comparator Control Register 0 */
#define CMP_CCR1     0x0C   /* RW  Comparator Control Register 1 */
#define CMP_CCR2     0x10   /* RW  Comparator Control Register 2 */
#define CMP_DCR      0x18   /* RW  DAC Control */
#define CMP_IER      0x1C   /* RW  Interrupt Enable */
#define CMP_CSR      0x20   /* RW  Comparator Status */
#define CMP_RRCR0    0x24   /* RW  Round Robin Control Register 0 */
#define CMP_RRCR1    0x28   /* RW  Round Robin Control Register 1 */
#define CMP_RRCSR    0x2C   /* RW  Round Robin Control and Status */
#define CMP_RRSR     0x30   /* RW  Round Robin Status */
#define CMP_RRCR2    0x38   /* RW  Round Robin Control Register 2 */

/* CSR fields. */
#define CMP_CSR_CFR     (1u << 0)   /* rising-edge flag, W1C */
#define CMP_CSR_CFF     (1u << 1)   /* falling-edge flag, W1C */
#define CMP_CSR_RRF     (1u << 2)   /* round-robin flag, W1C */
#define CMP_CSR_COUT    (1u << 8)   /* comparator output level, RO */
#define CMP_CSR_W1C_MASK  (CMP_CSR_CFR | CMP_CSR_CFF | CMP_CSR_RRF)

/* Constant RO values (RM reset values). */
#define CMP_VERID_VALUE   0x01000041u
#define CMP_PARAM_VALUE   0x00000002u

/* Non-zero RM reset value for a backed control register. */
#define CMP_CCR0_RESET    0x00000002u

static uint64_t mcxn_cmp_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNCMPState *s = MCXN_CMP(opaque);

    switch (offset) {
    case CMP_VERID:
        return CMP_VERID_VALUE;
    case CMP_PARAM:
        return CMP_PARAM_VALUE;
    case CMP_CSR:
        /*
         * No analog stimulus is simulated: the comparator output (COUT) reads
         * low and the edge/round-robin flags only ever hold whatever software
         * has not yet cleared.
         */
        return s->regs[CMP_CSR / 4] & ~CMP_CSR_COUT;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_cmp_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNCMPState *s = MCXN_CMP(opaque);

    switch (offset) {
    case CMP_VERID:
    case CMP_PARAM:
        /* Read-only registers: ignore writes. */
        return;
    case CMP_CSR:
        /* CFR/CFF/RRF are write-1-to-clear; COUT is read-only. */
        s->regs[CMP_CSR / 4] &= ~(value & CMP_CSR_W1C_MASK);
        return;
    default:
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_cmp_ops = {
    .read = mcxn_cmp_read,
    .write = mcxn_cmp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_cmp_reset(DeviceState *dev)
{
    MCXNCMPState *s = MCXN_CMP(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[CMP_CCR0 / 4] = CMP_CCR0_RESET;
}

static void mcxn_cmp_realize(DeviceState *dev, Error **errp)
{
    MCXNCMPState *s = MCXN_CMP(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_cmp_ops, s,
                          TYPE_MCXN_CMP, MCXN_CMP_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_cmp = {
    .name = TYPE_MCXN_CMP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNCMPState, MCXN_CMP_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_cmp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_cmp_realize;
    device_class_set_legacy_reset(dc, mcxn_cmp_reset);
    dc->vmsd = &vmstate_mcxn_cmp;
}

static const TypeInfo mcxn_cmp_types[] = {
    {
        .name          = TYPE_MCXN_CMP,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCMPState),
        .class_init    = mcxn_cmp_class_init,
    },
};

DEFINE_TYPES(mcxn_cmp_types)
