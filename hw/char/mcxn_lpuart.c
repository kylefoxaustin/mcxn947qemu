/*
 * NXP MCX N LP_FLEXCOMM / LPUART console model
 *
 * Bit masks and offsets taken verbatim from the MCXN947 CMSIS header
 * (devices/MCXN947/MCXN947_cm33_core0.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/char/mcxn_lpuart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"

/* --- LPUART core register offsets ------------------------------------------ */
#define LPUART_VERID    0x00  /* RO */
#define LPUART_PARAM    0x04  /* RO */
#define LPUART_GLOBAL   0x08
#define LPUART_PINCFG   0x0C
#define LPUART_BAUD     0x10
#define LPUART_STAT     0x14
#define LPUART_CTRL     0x18
#define LPUART_DATA     0x1C
#define LPUART_MATCH    0x20
#define LPUART_MODIR    0x24
#define LPUART_FIFO     0x28
#define LPUART_WATER    0x2C
#define LPUART_DATARO   0x30  /* RO */
#define LPUART_REIR     0x48  /* Receiver Extended Idle */
#define LPUART_TEIR     0x4C  /* Transmitter Extended Idle */
#define LPUART_HDCR     0x50  /* Half Duplex Control */
#define LPUART_TOCR     0x58  /* Timeout Control */
#define LPUART_TOSR     0x5C  /* Timeout Status */
#define LPUART_TIMEOUT0 0x60  /* Timeout 0..3 (step 4) */
#define LPUART_TIMEOUT3 0x6C

/* --- LP_FLEXCOMM wrapper register offsets ---------------------------------- */
#define LPFLEXCOMM_ISTAT   0xFF4  /* RO */
#define LPFLEXCOMM_PSELID  0xFF8

/* --- Bit masks (CMSIS) ----------------------------------------------------- */
#define STAT_OR     0x00080000u
#define STAT_IDLE   0x00100000u
#define STAT_RDRF   0x00200000u
#define STAT_TC     0x00400000u
#define STAT_TDRE   0x00800000u

#define CTRL_RE     0x00040000u
#define CTRL_TE     0x00080000u
#define CTRL_RIE    0x00200000u
#define CTRL_TCIE   0x00400000u
#define CTRL_TIE    0x00800000u

#define FIFO_RXFE   0x00000008u
#define FIFO_TXFE   0x00000080u
#define FIFO_RXEMPT 0x00400000u
#define FIFO_TXEMPT 0x00800000u

#define GLOBAL_RST  0x00000002u

#define PSELID_PERSEL   0x7u
#define PSELID_LOCK     0x8u
#define PERSEL_LPUART   1u    /* LP_FLEXCOMM_PERIPH_LPUART (CMSIS enum value) */
/*
 * Read-only capability bits in PSELID: a full LP_FLEXCOMM (as FlexComm4 is)
 * advertises LPUART/LPSPI/LPI2C present.  The MCUXpresso SDK gates LPUART_Init
 * on UARTPRESENT via LP_FLEXCOMM_PeripheralIsPresent() — without it the console
 * driver bails before programming BAUD/CTRL and PRINTF silently emits nothing.
 */
#define PSELID_UARTPRESENT  0x10u
#define PSELID_SPIPRESENT   0x20u
#define PSELID_I2CPRESENT   0x40u
#define PSELID_PRESENT_BITS \
    (PSELID_UARTPRESENT | PSELID_SPIPRESENT | PSELID_I2CPRESENT)

/*
 * VERID/PARAM are read by some HALs to size the FIFO.  Values are plausible
 * MCX-class constants; refine against the RM if a HAL ever depends on them.
 */
#define LPUART_VERID_VALUE  0x04010003u
#define LPUART_PARAM_VALUE  0x00000404u  /* TX/RX FIFO depth fields */

static void mcxn_lpuart_update_irq(MCXNLPUARTState *s)
{
    int level = 0;

    /* TX data-register-empty and transmit-complete are always asserted in this
     * model (writes are synchronous), so TIE/TCIE assert the line immediately. */
    if (s->ctrl & CTRL_TIE) {
        level = 1;
    }
    if (s->ctrl & CTRL_TCIE) {
        level = 1;
    }
    if ((s->ctrl & CTRL_RIE) && s->rx_full) {
        level = 1;
    }
    qemu_set_irq(s->irq, level);
}

static uint64_t mcxn_lpuart_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);
    uint32_t r = 0;

    switch (offset) {
    case LPUART_VERID:
        r = LPUART_VERID_VALUE;
        break;
    case LPUART_PARAM:
        r = LPUART_PARAM_VALUE;
        break;
    case LPUART_GLOBAL:
        r = s->global;
        break;
    case LPUART_PINCFG:
        r = s->pincfg;
        break;
    case LPUART_BAUD:
        r = s->baud;
        break;
    case LPUART_STAT:
        /* TX always ready; RDRF reflects the 1-byte rx holding register. */
        r = STAT_TDRE | STAT_TC;
        if (s->rx_full) {
            r |= STAT_RDRF;
        }
        break;
    case LPUART_CTRL:
        r = s->ctrl;
        break;
    case LPUART_DATA:
    case LPUART_DATARO:
        r = s->rx_byte;
        if (offset == LPUART_DATA && s->rx_full) {
            s->rx_full = false;
            mcxn_lpuart_update_irq(s);
        }
        break;
    case LPUART_MATCH:
        r = s->match;
        break;
    case LPUART_MODIR:
        r = s->modir;
        break;
    case LPUART_FIFO:
        r = s->fifo | FIFO_TXEMPT;
        if (!s->rx_full) {
            r |= FIFO_RXEMPT;
        }
        break;
    case LPUART_WATER:
        r = s->water;
        break;
    case LPUART_REIR:
        r = s->reir;
        break;
    case LPUART_TEIR:
        r = s->teir;
        break;
    case LPUART_HDCR:
        r = s->hdcr;
        break;
    case LPUART_TOCR:
        r = s->tocr;
        break;
    case LPUART_TOSR:
        r = s->tosr;
        break;
    case LPUART_TIMEOUT0 ... LPUART_TIMEOUT3:
        r = s->timeout[(offset - LPUART_TIMEOUT0) >> 2];
        break;
    case LPFLEXCOMM_ISTAT:
        r = 0;   /* no FlexComm-level interrupts modelled */
        break;
    case LPFLEXCOMM_PSELID:
        /* writable PERSEL/LOCK plus the RO present-capability bits */
        r = s->pselid | PSELID_PRESENT_BITS;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%03" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
    return r;
}

static void mcxn_lpuart_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);
    uint8_t ch;

    switch (offset) {
    case LPUART_GLOBAL:
        s->global = value;
        if (value & GLOBAL_RST) {
            /* Software reset: clear the model's writable state. */
            s->ctrl = s->baud = s->fifo = s->water = 0;
            s->rx_full = false;
            mcxn_lpuart_update_irq(s);
        }
        break;
    case LPUART_PINCFG:
        s->pincfg = value;
        break;
    case LPUART_BAUD:
        s->baud = value;
        break;
    case LPUART_STAT:
        /*
         * STAT is computed on read (TX always ready, RDRF tracks rx).  The
         * write-1-to-clear flag bits the SDK clears here have no standalone
         * model state, so accept and ignore the write.
         */
        break;
    case LPUART_CTRL:
        s->ctrl = value;
        mcxn_lpuart_update_irq(s);
        break;
    case LPUART_DATA:
        ch = value & 0xFF;
        /* Honour TE: only transmit when the transmitter is enabled. */
        if (s->ctrl & CTRL_TE) {
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        mcxn_lpuart_update_irq(s);
        break;
    case LPUART_MATCH:
        s->match = value;
        break;
    case LPUART_MODIR:
        s->modir = value;
        break;
    case LPUART_FIFO:
        s->fifo = value;
        break;
    case LPUART_WATER:
        s->water = value;
        break;
    case LPUART_REIR:
        s->reir = value;
        break;
    case LPUART_TEIR:
        s->teir = value;
        break;
    case LPUART_HDCR:
        s->hdcr = value;
        break;
    case LPUART_TOCR:
        s->tocr = value;
        break;
    case LPUART_TOSR:
        s->tosr = value;
        break;
    case LPUART_TIMEOUT0 ... LPUART_TIMEOUT3:
        s->timeout[(offset - LPUART_TIMEOUT0) >> 2] = value;
        break;
    case LPFLEXCOMM_PSELID:
        s->pselid = value & (PSELID_PERSEL | PSELID_LOCK);
        break;
    case LPUART_VERID:
    case LPUART_PARAM:
    case LPUART_DATARO:
    case LPFLEXCOMM_ISTAT:
        /* read-only */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%03" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps mcxn_lpuart_ops = {
    .read = mcxn_lpuart_read,
    .write = mcxn_lpuart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static int mcxn_lpuart_can_rx(void *opaque)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);
    /* Accept a byte only when the receiver is enabled and the holding reg is
     * empty (single-entry model). */
    return (s->ctrl & CTRL_RE) && !s->rx_full;
}

static void mcxn_lpuart_rx(void *opaque, const uint8_t *buf, int size)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);

    if (size > 0) {
        s->rx_byte = buf[0];
        s->rx_full = true;
        mcxn_lpuart_update_irq(s);
    }
}

static void mcxn_lpuart_reset(DeviceState *dev)
{
    MCXNLPUARTState *s = MCXN_LPUART(dev);

    s->global = s->pincfg = s->baud = s->ctrl = 0;
    s->match = s->modir = s->fifo = s->water = 0;
    s->pselid = PERSEL_LPUART;   /* default selection for a console instance */
    s->reir = s->teir = s->hdcr = s->tocr = s->tosr = 0;
    s->timeout[0] = s->timeout[1] = s->timeout[2] = s->timeout[3] = 0;
    s->rx_byte = 0;
    s->rx_full = false;
}

static void mcxn_lpuart_realize(DeviceState *dev, Error **errp)
{
    MCXNLPUARTState *s = MCXN_LPUART(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_lpuart_ops, s,
                          TYPE_MCXN_LPUART, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qemu_chr_fe_set_handlers(&s->chr, mcxn_lpuart_can_rx, mcxn_lpuart_rx,
                             NULL, NULL, s, NULL, true);
}

static const VMStateDescription vmstate_mcxn_lpuart = {
    .name = TYPE_MCXN_LPUART,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(global, MCXNLPUARTState),
        VMSTATE_UINT32(pincfg, MCXNLPUARTState),
        VMSTATE_UINT32(baud, MCXNLPUARTState),
        VMSTATE_UINT32(ctrl, MCXNLPUARTState),
        VMSTATE_UINT32(match, MCXNLPUARTState),
        VMSTATE_UINT32(modir, MCXNLPUARTState),
        VMSTATE_UINT32(fifo, MCXNLPUARTState),
        VMSTATE_UINT32(water, MCXNLPUARTState),
        VMSTATE_UINT32(pselid, MCXNLPUARTState),
        VMSTATE_UINT32(reir, MCXNLPUARTState),
        VMSTATE_UINT32(teir, MCXNLPUARTState),
        VMSTATE_UINT32(hdcr, MCXNLPUARTState),
        VMSTATE_UINT32(tocr, MCXNLPUARTState),
        VMSTATE_UINT32(tosr, MCXNLPUARTState),
        VMSTATE_UINT32_ARRAY(timeout, MCXNLPUARTState, 4),
        VMSTATE_UINT8(rx_byte, MCXNLPUARTState),
        VMSTATE_BOOL(rx_full, MCXNLPUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mcxn_lpuart_properties[] = {
    DEFINE_PROP_CHR("chardev", MCXNLPUARTState, chr),
};

static void mcxn_lpuart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_lpuart_realize;
    device_class_set_legacy_reset(dc, mcxn_lpuart_reset);
    dc->vmsd = &vmstate_mcxn_lpuart;
    device_class_set_props(dc, mcxn_lpuart_properties);
}

static const TypeInfo mcxn_lpuart_types[] = {
    {
        .name          = TYPE_MCXN_LPUART,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNLPUARTState),
        .class_init    = mcxn_lpuart_class_init,
    },
};

DEFINE_TYPES(mcxn_lpuart_types)
