/*
 * NXP MCX N SCG (System Clock Generator) — bring-up stub
 *
 * Register offsets/bits from the MCXN947 CMSIS header. All oscillator VLD and
 * PLL LOCK bits are bit 24 (0x0100_0000); reads of the *CSR registers OR that
 * bit in so firmware's "wait for valid/lock" polling completes. CSR (clock
 * status) mirrors RCCR so a clock-source switch reads back as taken.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_scg.h"
#include "migration/vmstate.h"

/* Register offsets */
#define SCG_VERID    0x000  /* RO */
#define SCG_PARAM    0x004  /* RO */
#define SCG_CSR      0x010  /* RO: current clock status */
#define SCG_RCCR     0x014  /* run clock control (selected source) */
#define SCG_SOSCCSR  0x100
#define SCG_SIRCCSR  0x200
#define SCG_FIRCCSR  0x300
#define SCG_ROSCCSR  0x400
#define SCG_APLLCSR  0x500
#define SCG_SPLLCSR  0x600

/* All VLD/LOCK status bits share bit 24. */
#define SCG_READY    0x01000000u

#define SCG_VERID_VALUE  0x06010000u

static uint64_t mcxn_scg_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNSCGState *s = MCXN_SCG(opaque);
    uint32_t v = (off < MCXN_SCG_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case SCG_VERID:
        return SCG_VERID_VALUE;
    case SCG_PARAM:
        return 0;
    case SCG_CSR:
        /* Report the source selected via RCCR as the active source. */
        return s->regs[SCG_RCCR >> 2];
    case SCG_SOSCCSR:
    case SCG_SIRCCSR:
    case SCG_FIRCCSR:
    case SCG_ROSCCSR:
    case SCG_APLLCSR:
    case SCG_SPLLCSR:
        return v | SCG_READY;   /* always valid/locked */
    default:
        return v;
    }
}

static void mcxn_scg_write(void *opaque, hwaddr off,
                           uint64_t value, unsigned size)
{
    MCXNSCGState *s = MCXN_SCG(opaque);

    if (off >= MCXN_SCG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    /* Read-only registers ignore writes. */
    if (off == SCG_VERID || off == SCG_PARAM || off == SCG_CSR) {
        return;
    }
    s->regs[off >> 2] = value;
}

static const MemoryRegionOps mcxn_scg_ops = {
    .read = mcxn_scg_read,
    .write = mcxn_scg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * SIRCCSR reset value (RM rev 7, SCG register map: "200h ... RW 0100_0020h").
 *
 * ⚠ bit 5 = SIRC_CLK_PERIPH_EN IS SET OUT OF RESET, and resetting this register to
 * ZERO -- which is what memset does -- BREAKS EVERY SDK CLOCK QUERY.
 *
 *     static uint32_t CLOCK_GetFro12MFreq(void) {
 *         return ((SCG0->SIRCCSR & SCG_SIRCCSR_SIRC_CLK_PERIPH_EN_MASK) != 0UL)
 *                ? 12000000U : 0U;
 *     }
 *
 * With the bit clear that returns 0 Hz, so CLOCK_GetLPFlexCommClkFreq() returns 0,
 * and LPI2C_SlaveInit()/LPUART_Init()/LPSPI_MasterInit() all hit
 *     assert(sourceClock_Hz > 0U)
 * and HARD-FAULT.  Nothing in the guest's clock_config.c ever sets this bit --
 * BECAUSE ON SILICON IT IS ALREADY SET.  A zero reset value is not a neutral
 * default; here it is a WRONG one, and it takes out an entire peripheral family.
 *
 * (Found by running the stock lpi2c/edma_b2b_transfer example, which asserted
 * "sourceClock_Hz > 0U" -- a driver telling me, in plain text, exactly what was
 * wrong.  bit 24 = SIRCVLD is already reported by the read path.)
 */
#define SCG_SIRCCSR_RESET  0x01000020u

static void mcxn_scg_reset(DeviceState *dev)
{
    MCXNSCGState *s = MCXN_SCG(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[SCG_SIRCCSR >> 2] = SCG_SIRCCSR_RESET;
}

static void mcxn_scg_realize(DeviceState *dev, Error **errp)
{
    MCXNSCGState *s = MCXN_SCG(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_scg_ops, s,
                          TYPE_MCXN_SCG, MCXN_SCG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_scg = {
    .name = TYPE_MCXN_SCG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSCGState, MCXN_SCG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_scg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_scg_realize;
    device_class_set_legacy_reset(dc, mcxn_scg_reset);
    dc->vmsd = &vmstate_mcxn_scg;
}

static const TypeInfo mcxn_scg_types[] = {
    {
        .name          = TYPE_MCXN_SCG,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSCGState),
        .class_init    = mcxn_scg_class_init,
    },
};

DEFINE_TYPES(mcxn_scg_types)
