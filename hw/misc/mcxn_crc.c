/*
 * NXP MCX N CRC (Cyclic Redundancy Check) engine — functional model.
 *
 * Implements the actual data path so firmware self-tests pass:
 *   - GPOLY (0x4) holds the generator polynomial (reset 0x0000_1021).
 *   - CTRL (0x8) selects width (TCRC: 0=CRC-16, 1=CRC-32), seed-vs-data
 *     (WAS), input/output bit/byte transposition (TOT/TOTR) and final XOR
 *     (FXOR).
 *   - DATA (0x0): a write with CTRL[WAS]=1 loads the seed; a write with
 *     CTRL[WAS]=0 feeds bytes into the engine (MSB-first, big-endian byte
 *     order as documented).  A read returns the running checksum with the
 *     TOTR transposition and FXOR complement applied.
 *
 * The engine is a plain MSB-first bit-serial CRC over the (optionally
 * transposed) input bytes, matching the hardware described in the MCX N
 * Reference Manual, chapter 40.
 *
 * Register offsets/bits from the MCXN947 CMSIS header (CRC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_crc.h"
#include "migration/vmstate.h"

/* Register offsets */
#define CRC_DATA   0x0   /* RW: seed/data/result */
#define CRC_GPOLY  0x4   /* RW: polynomial */
#define CRC_CTRL   0x8   /* RW: control */

/* CTRL fields */
#define CRC_CTRL_TCRC   (1u << 24)   /* 0:16-bit, 1:32-bit */
#define CRC_CTRL_WAS    (1u << 25)   /* write-as-seed */
#define CRC_CTRL_FXOR   (1u << 26)   /* complement read */
#define CRC_CTRL_TOTR   (3u << 28)   /* transpose type for read */
#define CRC_CTRL_TOT    (3u << 30)   /* transpose type for write */

#define CRC_CTRL_TOTR_SHIFT 28
#define CRC_CTRL_TOT_SHIFT  30

/* Reset values (RM chapter 40). */
#define CRC_GPOLY_RESET 0x00001021u

/* Reverse the bits within each byte of a 32-bit word. */
static uint32_t transpose_bits_in_bytes(uint32_t v)
{
    uint32_t r = 0;
    for (int b = 0; b < 4; b++) {
        uint8_t byte = (v >> (b * 8)) & 0xff;
        byte = revbit8(byte);
        r |= (uint32_t)byte << (b * 8);
    }
    return r;
}

/*
 * Apply the CRC transpose feature.  'type' is the 2-bit TOT/TOTR field:
 *   0: none
 *   1: bits in bytes transposed, bytes not
 *   2: bits in bytes and bytes transposed
 *   3: bytes transposed, bits in a byte not
 */
static uint32_t crc_transpose(uint32_t v, unsigned type)
{
    switch (type) {
    case 0:
        return v;
    case 1:
        return transpose_bits_in_bytes(v);
    case 2:
        return bswap32(transpose_bits_in_bytes(v));
    case 3:
        return bswap32(v);
    default:
        return v;
    }
}

/* MSB-first bit-serial CRC update of one byte. */
static uint32_t crc32_update_byte(uint32_t crc, uint8_t byte, uint32_t poly)
{
    crc ^= (uint32_t)byte << 24;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x80000000u) {
            crc = (crc << 1) ^ poly;
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

static uint32_t crc16_update_byte(uint32_t crc, uint8_t byte, uint32_t poly)
{
    crc ^= (uint32_t)byte << 8;
    for (int i = 0; i < 8; i++) {
        if (crc & 0x8000u) {
            crc = ((crc << 1) ^ poly) & 0xffffu;
        } else {
            crc = (crc << 1) & 0xffffu;
        }
    }
    return crc & 0xffffu;
}

/* Feed a 32-bit DATA write (after TOT transposition) into the engine. */
static void crc_feed_data(MCXNCRCState *s, uint32_t value, unsigned nbytes)
{
    bool is32 = s->ctrl & CRC_CTRL_TCRC;
    unsigned tot = (s->ctrl & CRC_CTRL_TOT) >> CRC_CTRL_TOT_SHIFT;
    uint32_t poly = is32 ? s->gpoly : (s->gpoly & 0xffffu);

    value = crc_transpose(value, tot);

    /* Bytes are consumed in big-endian (MSB-first) order. */
    for (unsigned b = 0; b < nbytes; b++) {
        uint8_t byte = (value >> ((nbytes - 1 - b) * 8)) & 0xff;
        if (is32) {
            s->data = crc32_update_byte(s->data, byte, poly);
        } else {
            s->data = crc16_update_byte(s->data, byte, poly);
        }
    }
}

static uint32_t crc_read_result(MCXNCRCState *s)
{
    bool is32 = s->ctrl & CRC_CTRL_TCRC;
    unsigned totr = (s->ctrl & CRC_CTRL_TOTR) >> CRC_CTRL_TOTR_SHIFT;
    uint32_t v = is32 ? s->data : (s->data & 0xffffu);

    v = crc_transpose(v, totr);

    if (s->ctrl & CRC_CTRL_FXOR) {
        v = is32 ? ~v : (~v & 0xffffu);
    }
    return v;
}

static uint64_t mcxn_crc_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNCRCState *s = MCXN_CRC(opaque);

    switch (offset) {
    case CRC_DATA:
        return crc_read_result(s);
    case CRC_GPOLY:
        return s->gpoly;
    case CRC_CTRL:
        return s->ctrl;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad read @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void mcxn_crc_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    MCXNCRCState *s = MCXN_CRC(opaque);

    switch (offset) {
    case CRC_DATA:
        if (s->ctrl & CRC_CTRL_WAS) {
            /* Load seed: stored as the initial accumulator value. */
            s->data = (s->ctrl & CRC_CTRL_TCRC) ? (uint32_t)value
                                                : ((uint32_t)value & 0xffffu);
        } else {
            crc_feed_data(s, (uint32_t)value, size);
        }
        break;
    case CRC_GPOLY:
        s->gpoly = value;
        break;
    case CRC_CTRL:
        s->ctrl = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad write @0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        break;
    }
}

static const MemoryRegionOps mcxn_crc_ops = {
    .read = mcxn_crc_read,
    .write = mcxn_crc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_crc_reset(DeviceState *dev)
{
    MCXNCRCState *s = MCXN_CRC(dev);

    s->data = 0;
    s->gpoly = CRC_GPOLY_RESET;
    s->ctrl = 0;
}

static void mcxn_crc_realize(DeviceState *dev, Error **errp)
{
    MCXNCRCState *s = MCXN_CRC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_crc_ops, s,
                          TYPE_MCXN_CRC, MCXN_CRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_crc = {
    .name = TYPE_MCXN_CRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(data, MCXNCRCState),
        VMSTATE_UINT32(gpoly, MCXNCRCState),
        VMSTATE_UINT32(ctrl, MCXNCRCState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_crc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_crc_realize;
    device_class_set_legacy_reset(dc, mcxn_crc_reset);
    dc->vmsd = &vmstate_mcxn_crc;
}

static const TypeInfo mcxn_crc_types[] = {
    {
        .name          = TYPE_MCXN_CRC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNCRCState),
        .class_init    = mcxn_crc_class_init,
    },
};

DEFINE_TYPES(mcxn_crc_types)
