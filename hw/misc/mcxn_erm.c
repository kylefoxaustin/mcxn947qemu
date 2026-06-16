/*
 * NXP MCX N ERM (Error Recovery Manager) — register-accurate model.
 *
 * The ERM aggregates RAM ECC error events: configuration (CR0, CR1), status
 * (SR0, SR1) and, per memory bank, an error-address register (EARn), a syndrome
 * register (SYNn) and a correctable-error counter (CORR_ERR_CNTn).
 *
 * In emulation no ECC error is ever raised, so:
 *   - The status registers SR0/SR1 are write-1-to-clear and stay 0 ("no error").
 *   - The error-address EARn and syndrome SYNn registers are read-only (CMSIS
 *     __I) and read 0.
 *   - CR0/CR1 and the CORR_ERR_CNTn counters are plain RW storage.
 *
 * All registers reset to 0 (RM section 30.5).  Offsets/access-types from the
 * MCXN947 CMSIS header (ERM_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/misc/mcxn_erm.h"
#include "migration/vmstate.h"

#define ERM_CR0   0x00   /* RW  Configuration Register 0 */
#define ERM_CR1   0x04   /* RW  Configuration Register 1 */
#define ERM_SR0   0x10   /* W1C Status Register 0 */
#define ERM_SR1   0x14   /* W1C Status Register 1 */

/*
 * Read-only (__I) per-bank error-address and syndrome registers.  The banks are
 * laid out at 0x100 + n*0x10 (EAR) and 0x104 + n*0x10 (SYN).  Treat any access
 * landing on those two sub-offsets within the bank window as read-only.
 */
#define ERM_BANK_BASE   0x100
#define ERM_BANK_STEP   0x10
#define ERM_BANK_EAR    0x0    /* EARn  within a bank: read-only */
#define ERM_BANK_SYN    0x4    /* SYNn  within a bank: read-only */
#define ERM_BANK_CNT    0x8    /* CORR_ERR_CNTn within a bank: RW */

static bool erm_is_readonly(hwaddr offset)
{
    if (offset >= ERM_BANK_BASE) {
        hwaddr sub = (offset - ERM_BANK_BASE) % ERM_BANK_STEP;

        /* EARn and SYNn are read-only; CORR_ERR_CNTn (and gaps) are not. */
        return sub == ERM_BANK_EAR || sub == ERM_BANK_SYN;
    }
    return false;
}

static uint64_t mcxn_erm_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNERMState *s = MCXN_ERM(opaque);

    return s->regs[offset / 4];
}

static void mcxn_erm_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNERMState *s = MCXN_ERM(opaque);

    switch (offset) {
    case ERM_SR0:
    case ERM_SR1:
        /* Write-1-to-clear status flags. */
        s->regs[offset / 4] &= ~(uint32_t)value;
        return;
    default:
        if (erm_is_readonly(offset)) {
            return;  /* EARn / SYNn are read-only */
        }
        s->regs[offset / 4] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_erm_ops = {
    .read = mcxn_erm_read,
    .write = mcxn_erm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_erm_reset(DeviceState *dev)
{
    MCXNERMState *s = MCXN_ERM(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void mcxn_erm_realize(DeviceState *dev, Error **errp)
{
    MCXNERMState *s = MCXN_ERM(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_erm_ops, s,
                          TYPE_MCXN_ERM, MCXN_ERM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_erm = {
    .name = TYPE_MCXN_ERM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNERMState, MCXN_ERM_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_erm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_erm_realize;
    device_class_set_legacy_reset(dc, mcxn_erm_reset);
    dc->vmsd = &vmstate_mcxn_erm;
}

static const TypeInfo mcxn_erm_types[] = {
    {
        .name          = TYPE_MCXN_ERM,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNERMState),
        .class_init    = mcxn_erm_class_init,
    },
};

DEFINE_TYPES(mcxn_erm_types)
