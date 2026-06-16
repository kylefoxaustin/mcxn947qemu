/*
 * NXP MCX N uSDHC (Ultra Secured Digital Host Controller) — bring-up model.
 *
 * Models just enough of the SD/MMC host for firmware init to make forward
 * progress without a real card backend:
 *
 *   - PRES_STATE (present state) reports the command line and data line idle
 *     (CIHB=0, CDIHB=0, DLA=0), the SD clock stable (SDSTB=1) and a card
 *     present (CINST=1), so "wait until not busy" and "is a card inserted"
 *     polling both resolve.
 *   - SYS_CTRL software-reset bits (RSTA/RSTC/RSTD) and the INITA
 *     initialization-active bit self-clear, so reset/init handshakes finish.
 *   - INT_STATUS is write-1-to-clear.  Writing a command to CMD_XFR_TYP raises
 *     command-complete (CC), and for a data command also transfer-complete
 *     (TC), so the "issue command, poll command-complete" loop terminates and
 *     the SD enumeration state machine advances.
 *   - HOST_CTRL_CAP advertises a plausible capability set.
 *
 * No actual card responses are produced; CMD_RSPx reads back zero.  Offsets and
 * bit masks come from the MCXN947 CMSIS header (USDHC_Type).  HOST_CTRL_CAP and
 * the reset-value constants are best-effort for this uSDHC revision (firmware
 * boot does not gate on the exact capability bits).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_usdhc.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS USDHC_Type). */
#define USDHC_DS_ADDR             0x00
#define USDHC_BLK_ATT             0x04
#define USDHC_CMD_ARG             0x08
#define USDHC_CMD_XFR_TYP         0x0C
#define USDHC_CMD_RSP0            0x10    /* RO */
#define USDHC_CMD_RSP1            0x14    /* RO */
#define USDHC_CMD_RSP2            0x18    /* RO */
#define USDHC_CMD_RSP3            0x1C    /* RO */
#define USDHC_DATA_BUFF_ACC_PORT  0x20
#define USDHC_PRES_STATE          0x24    /* RO */
#define USDHC_PROT_CTRL           0x28
#define USDHC_SYS_CTRL            0x2C
#define USDHC_INT_STATUS          0x30    /* W1C */
#define USDHC_INT_STATUS_EN       0x34
#define USDHC_INT_SIGNAL_EN       0x38
#define USDHC_AUTOCMD12_ERR_STATUS 0x3C
#define USDHC_HOST_CTRL_CAP       0x40
#define USDHC_WTMK_LVL            0x44
#define USDHC_MIX_CTRL            0x48
#define USDHC_FORCE_EVENT         0x50    /* WO */
#define USDHC_ADMA_ERR_STATUS     0x54    /* RO */
#define USDHC_ADMA_SYS_ADDR       0x58
#define USDHC_DLL_CTRL            0x60
#define USDHC_DLL_STATUS          0x64    /* RO */
#define USDHC_CLK_TUNE_CTRL_STATUS 0x68
#define USDHC_VEND_SPEC           0xC0
#define USDHC_MMC_BOOT            0xC4
#define USDHC_VEND_SPEC2          0xC8
#define USDHC_TUNING_CTRL         0xCC

/* PRES_STATE bits. */
#define PRES_CIHB    (1u << 0)   /* command inhibit (CMD line)  */
#define PRES_CDIHB   (1u << 1)   /* command inhibit (DATA line) */
#define PRES_DLA     (1u << 2)   /* data line active            */
#define PRES_SDSTB   (1u << 3)   /* SD clock stable             */
#define PRES_CINST   (1u << 16)  /* card inserted               */

/* SYS_CTRL bits. */
#define SYS_CTRL_RSTA   (1u << 24)
#define SYS_CTRL_RSTC   (1u << 25)
#define SYS_CTRL_RSTD   (1u << 26)
#define SYS_CTRL_INITA  (1u << 27)
#define SYS_CTRL_SELF_CLEAR \
    (SYS_CTRL_RSTA | SYS_CTRL_RSTC | SYS_CTRL_RSTD | SYS_CTRL_INITA)

/* CMD_XFR_TYP bits. */
#define CMD_XFR_DPSEL   (1u << 21)  /* data present select */

/* INT_STATUS bits. */
#define INT_CC   (1u << 0)   /* command complete  */
#define INT_TC   (1u << 1)   /* transfer complete */

/* PRES_STATE static reset content: card present and clock stable, lines idle. */
#define PRES_STATE_VALUE  (PRES_CINST | PRES_SDSTB)

/* Best-effort capability advertisement (not boot-gating). */
#define HOST_CTRL_CAP_VALUE  0x07F30000u

static void mcxn_usdhc_update_irq(MCXNUSDHCState *s)
{
    uint32_t active = s->regs[USDHC_INT_STATUS >> 2] &
                      s->regs[USDHC_INT_SIGNAL_EN >> 2];
    qemu_set_irq(s->irq, active != 0);
}

static uint64_t mcxn_usdhc_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSDHCState *s = MCXN_USDHC(opaque);
    uint32_t v = (off < MCXN_USDHC_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case USDHC_PRES_STATE:
        /*
         * Command and data lines are always idle in the model so any
         * "wait while inhibited" loop exits immediately.  Card is present
         * and the SD clock reads stable.
         */
        return PRES_STATE_VALUE;
    case USDHC_HOST_CTRL_CAP:
        return HOST_CTRL_CAP_VALUE;
    case USDHC_CMD_RSP0:
    case USDHC_CMD_RSP1:
    case USDHC_CMD_RSP2:
    case USDHC_CMD_RSP3:
        return 0;   /* no card responses modelled */
    case USDHC_ADMA_ERR_STATUS:
    case USDHC_DLL_STATUS:
        return 0;
    case USDHC_SYS_CTRL:
        /* Self-clearing reset/init bits never read back as set. */
        return v & ~SYS_CTRL_SELF_CLEAR;
    default:
        return v;
    }
}

static void mcxn_usdhc_write(void *opaque, hwaddr off, uint64_t value,
                             unsigned size)
{
    MCXNUSDHCState *s = MCXN_USDHC(opaque);
    uint32_t val = value;

    if (off >= MCXN_USDHC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case USDHC_PRES_STATE:
    case USDHC_CMD_RSP0:
    case USDHC_CMD_RSP1:
    case USDHC_CMD_RSP2:
    case USDHC_CMD_RSP3:
    case USDHC_ADMA_ERR_STATUS:
    case USDHC_DLL_STATUS:
        return;   /* read-only */
    case USDHC_HOST_CTRL_CAP:
        return;   /* capabilities are fixed */
    case USDHC_SYS_CTRL:
        /* Latch the value but drop the self-clearing reset/init bits. */
        s->regs[off >> 2] = val & ~SYS_CTRL_SELF_CLEAR;
        if (val & (SYS_CTRL_RSTA | SYS_CTRL_RSTC | SYS_CTRL_RSTD)) {
            /* A reset clears pending interrupt status. */
            s->regs[USDHC_INT_STATUS >> 2] = 0;
            mcxn_usdhc_update_irq(s);
        }
        return;
    case USDHC_INT_STATUS:
        /* Write-1-to-clear. */
        s->regs[off >> 2] &= ~val;
        mcxn_usdhc_update_irq(s);
        return;
    case USDHC_CMD_XFR_TYP:
        /*
         * Issuing a command completes instantly: raise command-complete, and
         * for a data command also transfer-complete, so the poll loop ends.
         */
        s->regs[off >> 2] = val;
        s->regs[USDHC_INT_STATUS >> 2] |= INT_CC;
        if (val & CMD_XFR_DPSEL) {
            s->regs[USDHC_INT_STATUS >> 2] |= INT_TC;
        }
        mcxn_usdhc_update_irq(s);
        return;
    case USDHC_INT_SIGNAL_EN:
        s->regs[off >> 2] = val;
        mcxn_usdhc_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = val;
        return;
    }
}

static const MemoryRegionOps mcxn_usdhc_ops = {
    .read = mcxn_usdhc_read,
    .write = mcxn_usdhc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_usdhc_reset(DeviceState *dev)
{
    MCXNUSDHCState *s = MCXN_USDHC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[USDHC_HOST_CTRL_CAP >> 2] = HOST_CTRL_CAP_VALUE;
}

static void mcxn_usdhc_realize(DeviceState *dev, Error **errp)
{
    MCXNUSDHCState *s = MCXN_USDHC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_usdhc_ops, s,
                          TYPE_MCXN_USDHC, MCXN_USDHC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_usdhc = {
    .name = TYPE_MCXN_USDHC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSDHCState, MCXN_USDHC_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_usdhc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_usdhc_realize;
    device_class_set_legacy_reset(dc, mcxn_usdhc_reset);
    dc->vmsd = &vmstate_mcxn_usdhc;
}

static const TypeInfo mcxn_usdhc_types[] = {
    {
        .name          = TYPE_MCXN_USDHC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNUSDHCState),
        .class_init    = mcxn_usdhc_class_init,
    },
};

DEFINE_TYPES(mcxn_usdhc_types)
