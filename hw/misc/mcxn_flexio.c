/*
 * NXP MCX N FlexIO (Flexible I/O) — bring-up model.
 *
 * FlexIO is a configurable engine built from shifters, timers and pins.  This
 * model backs the whole register window, returns the read-only VERID/PARAM
 * identification constants from the reference manual, and keeps the status
 * registers (SHIFTSTAT, SHIFTERR, TIMSTAT, TRGSTAT, PINSTAT) at their idle reset
 * value so firmware bring-up never blocks.  The status registers are
 * write-1-to-clear in hardware; writes here clear the addressed bits.
 *
 * CTRL[SWRST] (bit 1) is honoured: writing it re-applies the reset state, and it
 * reads back as 0 (self-clearing) so a software-reset poll completes.
 *
 * Offsets from the MCXN947 CMSIS header (FLEXIO_Type); reset values from RM
 * chapter 71 (FlexIO register descriptions).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_flexio.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS FLEXIO_Type). */
#define R_VERID      0x000  /* RO */
#define R_PARAM      0x004  /* RO */
#define R_CTRL       0x008
#define R_PIN        0x00C  /* RO live pin state */
#define R_SHIFTSTAT  0x010  /* W1C status */
#define R_SHIFTERR   0x014  /* W1C status */
#define R_TIMSTAT    0x018  /* W1C status */
#define R_SHIFTSIEN  0x020
#define R_SHIFTEIEN  0x024
#define R_TIMIEN     0x028
#define R_SHIFTSDEN  0x030
#define R_TIMERSDEN  0x038
#define R_SHIFTSTATE 0x040
#define R_TRGSTAT    0x048  /* W1C status */
#define R_TRIGIEN    0x04C
#define R_PINSTAT    0x050  /* W1C status */
#define R_PINIEN     0x054
#define R_PINREN     0x058
#define R_PINFEN     0x05C
#define R_PINOUTD    0x060
#define R_PINOUTE    0x064
#define R_PINOUTDIS  0x068  /* WO */
#define R_PINOUTCLR  0x06C  /* WO */
#define R_PINOUTSET  0x070  /* WO */
#define R_PINOUTTOG  0x074  /* WO */

/* Register-array blocks (8 entries, 4-byte step). */
#define R_SHIFTCTL    0x080
#define R_SHIFTCFG    0x100
#define R_SHIFTBUF    0x200
#define R_SHIFTBUFBIS 0x280
#define R_SHIFTBUFBYS 0x300
#define R_SHIFTBUFBBS 0x380
#define R_TIMCTL      0x400
#define R_TIMCFG      0x480
#define R_TIMCMP      0x500
#define R_SHIFTBUFNBS 0x680
#define R_SHIFTBUFHWS 0x700
#define R_SHIFTBUFNIS 0x780
#define R_SHIFTBUFOES 0x800
#define R_SHIFTBUFEOS 0x880
#define R_SHIFTBUFHBS 0x900

/* Read-only identification reset constants (RM chapter 71). */
#define FLEXIO_VERID_RST 0x02010003u
#define FLEXIO_PARAM_RST 0x08200808u

/* CTRL fields. */
#define FLEXIO_CTRL_FLEXEN (1u << 0)
#define FLEXIO_CTRL_SWRST  (1u << 1)

/*
 * FlexIO-as-SPI-master (functional model).
 *
 * FlexIO has no upstream device to drive -- it is a bare shifter/timer/pin fabric whose
 * job is to EMULATE a serial peripheral in software.  Configured as an SPI master (a
 * transmit shifter feeding MOSI, a receive shifter capturing MISO, a timer clocking SCK),
 * it is here made to drive a REAL m25p80 SPI-NOR on an SSI bus: a byte written to a
 * transmit shifter's buffer is shifted onto the bus (ssi_transfer) and the byte shifted in
 * lands in the receive shifter's buffer.  The oracle is the flash's own JEDEC ID / stored
 * data -- not a loopback echo.
 *
 * ⚠ Scope, stated: the SHIFTER DATAPATH is modelled functionally (one byte per buffer
 * write, MSB/LSB order per the SHIFTBUF vs SHIFTBUFBIS alias), NOT bit-by-bit on the
 * timer's SCK edges -- the timer (TIMCTL/TIMCFG/TIMCMP) configures the transfer but its
 * cycle-level clocking is abstracted, exactly as every serial TX in this tree abstracts
 * the baud generator.  The chip-select is a FlexIO OUTPUT PIN (a board seam: the board
 * wires FlexIO pin FLEXIO_SPI_CS_PIN to the NOR's CS); the guest drives it via the PINOUT
 * registers.  Only the SPI (transmit+receive shifter) configuration is modelled; the
 * UART/I2S/PWM/motor shifter+timer modes are not.
 */
#define FLEXIO_NSHIFTER      8
#define SHIFTCTL_SMOD_MASK   0x7u
#define SHIFTCTL_SMOD_RX     1u   /* receive: capture the shifter into SHIFTBUF   */
#define SHIFTCTL_SMOD_TX     2u   /* transmit: load SHIFTBUF into the shifter     */
#define FLEXIO_SPI_CS_PIN    4    /* board seam: FlexIO pin 4 -> NOR chip-select  */

static uint8_t flexio_bitrev8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

static void mcxn_flexio_update_irq(MCXNFlexIOState *s)
{
    bool active = (s->regs[R_SHIFTSTAT / 4] & s->regs[R_SHIFTSIEN / 4]) != 0 ||
                  (s->regs[R_TIMSTAT / 4]   & s->regs[R_TIMIEN / 4])    != 0;
    qemu_set_irq(s->irq, active);
}

/* The receive shifter index (first with SMOD=receive), or -1. */
static int flexio_rx_shifter(MCXNFlexIOState *s)
{
    int i;

    for (i = 0; i < FLEXIO_NSHIFTER; i++) {
        if ((s->regs[(R_SHIFTCTL + i * 4) / 4] & SHIFTCTL_SMOD_MASK) ==
            SHIFTCTL_SMOD_TX) {
            continue;
        }
        if ((s->regs[(R_SHIFTCTL + i * 4) / 4] & SHIFTCTL_SMOD_MASK) ==
            SHIFTCTL_SMOD_RX) {
            return i;
        }
    }
    return -1;
}

/*
 * A transmit-shifter buffer write shifts one byte out the SSI bus and captures the byte
 * shifted in.  `bis` = the SHIFTBUFBIS (bit-swapped) alias, the MSB-first form SPI uses;
 * the plain SHIFTBUF alias is LSB-first, so the on-wire byte is bit-reversed.
 */
static void mcxn_flexio_spi_tx(MCXNFlexIOState *s, int tx_i, uint32_t value, bool bis)
{
    uint8_t tx = value & 0xFF;
    int rx_i;
    uint8_t rx;

    if (!s->spi_bus || !(s->regs[R_CTRL / 4] & FLEXIO_CTRL_FLEXEN)) {
        return;
    }
    if ((s->regs[(R_SHIFTCTL + tx_i * 4) / 4] & SHIFTCTL_SMOD_MASK) !=
        SHIFTCTL_SMOD_TX) {
        return;                                  /* not a transmit shifter */
    }
    rx = ssi_transfer(s->spi_bus, bis ? tx : flexio_bitrev8(tx));
    if (!bis) {
        rx = flexio_bitrev8(rx);
    }
    rx_i = flexio_rx_shifter(s);
    if (rx_i >= 0) {
        /* Deposit in both aliases so a read of either returns the byte in its order. */
        s->regs[(R_SHIFTBUF + rx_i * 4) / 4] = flexio_bitrev8(rx);
        s->regs[(R_SHIFTBUFBIS + rx_i * 4) / 4] = rx;
        s->regs[R_SHIFTSTAT / 4] |= (1u << rx_i);   /* RX buffer full */
    }
    s->regs[R_SHIFTSTAT / 4] |= (1u << tx_i);       /* TX buffer empty */
    mcxn_flexio_update_irq(s);
}

/* Drive the chip-select gpio from the FlexIO output-pin data (active low). */
static void mcxn_flexio_update_cs(MCXNFlexIOState *s)
{
    uint32_t d = s->regs[R_PINOUTD / 4];

    qemu_set_irq(s->spi_cs, (d >> FLEXIO_SPI_CS_PIN) & 1);
}

static bool flexio_is_readonly(hwaddr off)
{
    switch (off) {
    case R_VERID:
    case R_PARAM:
    case R_PIN:
        return true;
    default:
        return false;
    }
}

static bool flexio_is_w1c(hwaddr off)
{
    switch (off) {
    case R_SHIFTSTAT:
    case R_SHIFTERR:
    case R_TIMSTAT:
    case R_TRGSTAT:
    case R_PINSTAT:
        return true;
    default:
        return false;
    }
}

static void mcxn_flexio_reset(DeviceState *dev)
{
    MCXNFlexIOState *s = MCXN_FLEXIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_VERID / 4] = FLEXIO_VERID_RST;
    s->regs[R_PARAM / 4] = FLEXIO_PARAM_RST;
}

static uint64_t mcxn_flexio_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNFlexIOState *s = MCXN_FLEXIO(opaque);
    hwaddr idx = off & ~0x3u;
    uint32_t reg, shift;

    if (off >= MCXN_FLEXIO_SIZE) {
        return 0;
    }
    reg = s->regs[idx >> 2];

    /* SHIFTSTAT: a transmit shifter always reports "buffer empty" (room) -- the SDK polls
     * this before loading the next byte; receive bits are the stored flags set on a byte. */
    if (idx == R_SHIFTSTAT) {
        int i;

        for (i = 0; i < FLEXIO_NSHIFTER; i++) {
            if ((s->regs[(R_SHIFTCTL + i * 4) / 4] & SHIFTCTL_SMOD_MASK) ==
                SHIFTCTL_SMOD_TX) {
                reg |= (1u << i);
            }
        }
    }

    /* Reading a receive shifter's buffer consumes the byte: clear its SHIFTSTAT flag. */
    if ((idx >= R_SHIFTBUF && idx < R_SHIFTBUF + FLEXIO_NSHIFTER * 4) ||
        (idx >= R_SHIFTBUFBIS && idx < R_SHIFTBUFBIS + FLEXIO_NSHIFTER * 4)) {
        int base = (idx >= R_SHIFTBUFBIS) ? R_SHIFTBUFBIS : R_SHIFTBUF;
        int i = (idx - base) / 4;

        if ((s->regs[(R_SHIFTCTL + i * 4) / 4] & SHIFTCTL_SMOD_MASK) ==
            SHIFTCTL_SMOD_RX) {
            s->regs[R_SHIFTSTAT / 4] &= ~(1u << i);
            mcxn_flexio_update_irq(s);
        }
    }

    shift = (off & 0x3u) * 8;
    return (reg >> shift) & ((size == 4) ? 0xFFFFFFFFu
                                         : ((1u << (size * 8)) - 1));
}

static void mcxn_flexio_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size)
{
    MCXNFlexIOState *s = MCXN_FLEXIO(opaque);
    hwaddr idx = off & ~0x3u;
    uint32_t shift, mask, cur, v;

    if (off >= MCXN_FLEXIO_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }
    if (flexio_is_readonly(idx)) {
        return;
    }

    shift = (off & 0x3u) * 8;
    mask = ((size == 4) ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1)) << shift;
    cur = s->regs[idx >> 2];
    v = (cur & ~mask) | ((uint32_t)(value << shift) & mask);

    /* Write-1-to-clear status registers: clear only the bits written as 1. */
    if (flexio_is_w1c(idx)) {
        uint32_t w1c = (uint32_t)(value << shift) & mask;
        s->regs[idx >> 2] = cur & ~w1c;
        return;
    }

    if (idx == R_CTRL) {
        if (v & FLEXIO_CTRL_SWRST) {
            /* Software reset: re-apply reset state, then leave SWRST cleared so
             * a poll on the bit completes. */
            mcxn_flexio_reset(DEVICE(s));
            return;
        }
        s->regs[idx >> 2] = v;
        return;
    }

    /* Pin-output SET/CLR/TOG aliases modify PINOUTD; then re-drive the chip-select. */
    {
        uint32_t written = (uint32_t)(value << shift) & mask;

        if (idx == R_PINOUTSET) {
            s->regs[R_PINOUTD / 4] |=  written;
            mcxn_flexio_update_cs(s);
            return;
        }
        if (idx == R_PINOUTCLR) {
            s->regs[R_PINOUTD / 4] &= ~written;
            mcxn_flexio_update_cs(s);
            return;
        }
        if (idx == R_PINOUTTOG) {
            s->regs[R_PINOUTD / 4] ^=  written;
            mcxn_flexio_update_cs(s);
            return;
        }
    }

    s->regs[idx >> 2] = v;

    if (idx == R_PINOUTD) {
        mcxn_flexio_update_cs(s);
        return;
    }
    /* A write to a transmit shifter's buffer shifts a byte over the SSI bus (SPI). */
    if (idx >= R_SHIFTBUF && idx < R_SHIFTBUF + FLEXIO_NSHIFTER * 4) {
        mcxn_flexio_spi_tx(s, (idx - R_SHIFTBUF) / 4, v, false);   /* LSB-first alias */
    } else if (idx >= R_SHIFTBUFBIS && idx < R_SHIFTBUFBIS + FLEXIO_NSHIFTER * 4) {
        mcxn_flexio_spi_tx(s, (idx - R_SHIFTBUFBIS) / 4, v, true); /* MSB-first alias */
    }
}

static const MemoryRegionOps mcxn_flexio_ops = {
    .read = mcxn_flexio_read,
    .write = mcxn_flexio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_flexio_realize(DeviceState *dev, Error **errp)
{
    MCXNFlexIOState *s = MCXN_FLEXIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_flexio_ops, s,
                          TYPE_MCXN_FLEXIO, MCXN_FLEXIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);       /* 0: NVIC interrupt        */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->spi_cs);    /* 1: SPI chip-select (pin) */
    /* FlexIO-as-SPI: a real SSI bus the SoC attaches an m25p80 NOR to. */
    s->spi_bus = ssi_create_bus(dev, "flexio-spi");
}

static const VMStateDescription vmstate_mcxn_flexio = {
    .name = TYPE_MCXN_FLEXIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNFlexIOState, MCXN_FLEXIO_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_flexio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_flexio_realize;
    device_class_set_legacy_reset(dc, mcxn_flexio_reset);
    dc->vmsd = &vmstate_mcxn_flexio;
}

static const TypeInfo mcxn_flexio_types[] = {
    {
        .name          = TYPE_MCXN_FLEXIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNFlexIOState),
        .class_init    = mcxn_flexio_class_init,
    },
};

DEFINE_TYPES(mcxn_flexio_types)
