/*
 * NXP MCX N FlexSPI — functional model with a real SPI-NOR behind it.  See
 * header for the data path and the silent-wrong it exists to kill.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/misc/mcxn_flexspi.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS FLEXSPI_Type). */
#define FLEXSPI_MCR0        0x00
#define FLEXSPI_AHBCR       0x0C
#define FLEXSPI_INTEN       0x10
#define FLEXSPI_INTR        0x14    /* W1C status */
#define FLEXSPI_IPCR0       0xA0    /* SFAR: serial flash address */
#define FLEXSPI_IPCR1       0xA4    /* IDATSZ [15:0], ISEQID [19:16] */
#define FLEXSPI_IPCMD       0xB0    /* WO trigger */
#define FLEXSPI_IPRXFCR     0xB8
#define FLEXSPI_IPTXFCR     0xBC
#define FLEXSPI_STS0        0xE0    /* RO */
#define FLEXSPI_STS1        0xE4    /* RO */
#define FLEXSPI_STS2        0xE8    /* RO */
#define FLEXSPI_AHBSPNDSTS  0xEC    /* RO */
#define FLEXSPI_IPRXFSTS    0xF0    /* RO */
#define FLEXSPI_IPTXFSTS    0xF4    /* RO */
#define FLEXSPI_RFDR0       0x100   /* RFDR[32] @0x100..0x17C, RO */
#define FLEXSPI_TFDR0       0x180   /* TFDR[32] @0x180..0x1FC, WO */
#define FLEXSPI_LUT0        0x200   /* LUT[64]  @0x200..0x2FC       */
#define FLEXSPI_LUT_LAST    (FLEXSPI_LUT0 + 64 * 4)

/* MCR0 / INTR / IPCMD bits. */
#define MCR0_SWRESET   (1u << 0)
#define INTR_IPCMDDONE (1u << 0)
#define INTR_IPCMDERR  (1u << 3)
#define INTR_IPRXWA    (1u << 5)   /* RX FIFO has data for the guest   */
#define INTR_IPTXWE    (1u << 6)   /* TX FIFO wants data from the guest */
#define IPCMD_TRG      (1u << 0)

/* STS0: report the controller idle so the pre-command wait passes. */
#define STS0_SEQIDLE   (1u << 0)
#define STS0_ARBIDLE   (1u << 1)
#define STS0_IDLE      (STS0_SEQIDLE | STS0_ARBIDLE)

/* IPRXFCR / IPTXFCR. */
#define FCR_CLRF       (1u << 0)
#define FCR_WMRK(v)    (((v) >> 2) & 0x7F)   /* watermark, in 64-bit entries */

/* IPCR1 fields. */
#define IPCR1_IDATSZ(v) ((v) & 0xFFFF)
#define IPCR1_ISEQID(v) (((v) >> 16) & 0xF)

/*
 * LUT instruction encoding (CMSIS FLEXSPI_LUT_*; SDK fsl_flexspi.h).  Each LUT
 * register holds two instructions: OPCODE[15:10] OPERAND[7:0], then
 * OPCODE[31:26] OPERAND[23:16].  A sequence is 4 consecutive LUT registers.
 */
#define LUT_OPCODE0(v)  (((v) >> 10) & 0x3F)
#define LUT_OPERAND0(v) ((v) & 0xFF)
#define LUT_OPCODE1(v)  (((v) >> 26) & 0x3F)
#define LUT_OPERAND1(v) (((v) >> 16) & 0xFF)

#define LUT_CMD_STOP    0x00
#define LUT_CMD_SDR     0x01   /* operand = the SPI-NOR opcode */
#define LUT_CMD_DDR     0x21

/* SPI-NOR opcodes (W25Q64). */
#define NOR_WRDI        0x04
#define NOR_RDSR1       0x05
#define NOR_WREN        0x06
#define NOR_READ        0x03
#define NOR_FAST_READ   0x0B
#define NOR_DUAL_READ   0x3B
#define NOR_QUAD_READ   0x6B
#define NOR_QUAD_IO_RD  0xEB
#define NOR_PP          0x02
#define NOR_QPP         0x32
#define NOR_SE          0x20   /* 4 KiB sector erase   */
#define NOR_BE32        0x52   /* 32 KiB block erase   */
#define NOR_BE64        0xD8   /* 64 KiB block erase   */
#define NOR_CE1         0xC7   /* chip erase           */
#define NOR_CE2         0x60
#define NOR_RDID        0x9F   /* JEDEC ID             */

/* Winbond W25Q64: manufacturer 0xEF, type 0x40, capacity 0x17 (2^23 = 8 MiB). */
static const uint8_t nor_jedec_id[3] = { 0xEF, 0x40, 0x17 };

static uint8_t *nor_ptr(MCXNFlexSPIState *s)
{
    return memory_region_get_ram_ptr(&s->nor);
}

/* SFAR may be a flash offset or an address in either AHB aperture. */
static uint32_t nor_off(MCXNFlexSPIState *s, uint32_t addr)
{
    return s->flash_size ? (addr & (uint32_t)(s->flash_size - 1)) : 0;
}

static void mcxn_flexspi_update_irq(MCXNFlexSPIState *s)
{
    uint32_t active = s->regs[FLEXSPI_INTR >> 2] & s->regs[FLEXSPI_INTEN >> 2];

    qemu_set_irq(s->irq, active != 0);
}

static void flexspi_done(MCXNFlexSPIState *s)
{
    s->regs[FLEXSPI_INTR >> 2] |= INTR_IPCMDDONE;
    mcxn_flexspi_update_irq(s);
}

static void flexspi_error(MCXNFlexSPIState *s)
{
    s->regs[FLEXSPI_INTR >> 2] |= INTR_IPCMDERR | INTR_IPCMDDONE;
    mcxn_flexspi_update_irq(s);
}

/* Erase [off, off+len) to 0xFF. */
static void nor_erase(MCXNFlexSPIState *s, uint32_t off, uint32_t len)
{
    uint8_t *nor = nor_ptr(s);

    off &= ~(len - 1);
    if (off + len > s->flash_size) {
        return;
    }
    memset(nor + off, 0xFF, len);
    memory_region_flush_rom_device(&s->nor, off, len);
}

/*
 * Program the staged TX data.  NOR flash only clears bits, so this is an AND —
 * programming a location that was not erased first corrupts it, exactly as it
 * does on silicon (a NOR reports no error for this, which is why the model must
 * not quietly "succeed" by overwriting).  A page program also wraps inside its
 * 256-byte page rather than running on into the next one.
 */
static void nor_program(MCXNFlexSPIState *s, uint32_t off, const uint8_t *data,
                        uint32_t len)
{
    uint8_t *nor = nor_ptr(s);
    uint32_t page = off & ~(uint32_t)(MCXN_NOR_PAGE - 1);

    for (uint32_t i = 0; i < len; i++) {
        uint32_t a = page + ((off + i - page) % MCXN_NOR_PAGE);

        if (a >= s->flash_size) {
            continue;
        }
        nor[a] &= data[i];
    }
    memory_region_flush_rom_device(&s->nor, page, MCXN_NOR_PAGE);
}

/* Hand the guest a read result through the IP RX FIFO. */
static void flexspi_rx_load(MCXNFlexSPIState *s, const uint8_t *data,
                            uint32_t len)
{
    len = MIN(len, MCXN_FLEXSPI_XFER_MAX);
    memcpy(s->rx_buf, data, len);
    s->rx_len = len;
    s->rx_pos = 0;
    if (len) {
        s->regs[FLEXSPI_INTR >> 2] |= INTR_IPRXWA;
    }
}

/*
 * Decode the LUT sequence selected by IPCR1[ISEQID] and return the SPI-NOR
 * opcode it issues (its first CMD_SDR/CMD_DDR operand), or -1 if it has none.
 */
static int flexspi_lut_opcode(MCXNFlexSPIState *s, uint32_t seq)
{
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t lut = s->regs[(FLEXSPI_LUT0 >> 2) + seq * 4 + i];
        uint32_t op0 = LUT_OPCODE0(lut), op1 = LUT_OPCODE1(lut);

        if (op0 == LUT_CMD_SDR || op0 == LUT_CMD_DDR) {
            return LUT_OPERAND0(lut);
        }
        if (op0 == LUT_CMD_STOP) {
            break;
        }
        if (op1 == LUT_CMD_SDR || op1 == LUT_CMD_DDR) {
            return LUT_OPERAND1(lut);
        }
        if (op1 == LUT_CMD_STOP) {
            break;
        }
    }
    return -1;
}

/* Commit a program once the guest has fed all of its data through TFDR. */
static void flexspi_program_commit(MCXNFlexSPIState *s)
{
    nor_program(s, s->pgm_addr, s->tx_buf, s->pgm_len);
    s->wel = false;                 /* the latch clears after the operation */
    s->pgm_pending = false;
    s->regs[FLEXSPI_INTR >> 2] &= ~INTR_IPTXWE;
    flexspi_done(s);
}

/* IPCMD[TRG]: run the sequence in LUT[ISEQID] against the NOR. */
static void flexspi_ip_command(MCXNFlexSPIState *s)
{
    uint32_t ipcr1 = s->regs[FLEXSPI_IPCR1 >> 2];
    uint32_t datsz = IPCR1_IDATSZ(ipcr1);
    uint32_t seq   = IPCR1_ISEQID(ipcr1);
    uint32_t addr  = nor_off(s, s->regs[FLEXSPI_IPCR0 >> 2]);
    int cmd = flexspi_lut_opcode(s, seq);
    uint8_t sr;

    s->rx_len = s->rx_pos = 0;
    s->regs[FLEXSPI_INTR >> 2] &= ~(INTR_IPRXWA | INTR_IPTXWE);

    if (cmd < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mcxn-flexspi: IP command with no CMD in LUT seq %u\n",
                      seq);
        flexspi_error(s);
        return;
    }
    if (datsz > MCXN_FLEXSPI_XFER_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "mcxn-flexspi: IP transfer of %u bytes exceeds the "
                      "modelled maximum (%u)\n", datsz, MCXN_FLEXSPI_XFER_MAX);
        flexspi_error(s);
        return;
    }

    switch (cmd) {
    case NOR_WREN:
        s->wel = true;
        flexspi_done(s);
        return;

    case NOR_WRDI:
        s->wel = false;
        flexspi_done(s);
        return;

    case NOR_RDSR1:
        /* BUSY (bit 0) is always clear: our operations complete instantly. */
        sr = s->wel ? 0x02 : 0x00;
        flexspi_rx_load(s, &sr, 1);
        flexspi_done(s);
        return;

    case NOR_RDID:
        flexspi_rx_load(s, nor_jedec_id, sizeof(nor_jedec_id));
        flexspi_done(s);
        return;

    case NOR_READ:
    case NOR_FAST_READ:
    case NOR_DUAL_READ:
    case NOR_QUAD_READ:
    case NOR_QUAD_IO_RD:
        if (addr + datsz > s->flash_size) {
            flexspi_error(s);
            return;
        }
        flexspi_rx_load(s, nor_ptr(s) + addr, datsz);
        flexspi_done(s);
        return;

    case NOR_PP:
    case NOR_QPP:
        if (!s->wel) {
            /* No write-enable latch: the NOR ignores the program. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mcxn-flexspi: page program without WREN is ignored\n");
            flexspi_done(s);
            return;
        }
        /* The SDK triggers the command and only then feeds TFDR, so stage the
         * program and ask for data (IPTXWE); commit on the last word. */
        s->pgm_pending = true;
        s->pgm_addr = addr;
        s->pgm_len = datsz;
        s->tx_len = 0;
        if (datsz == 0) {
            flexspi_program_commit(s);
        } else {
            s->regs[FLEXSPI_INTR >> 2] |= INTR_IPTXWE;
            mcxn_flexspi_update_irq(s);
        }
        return;

    case NOR_SE:
    case NOR_BE32:
    case NOR_BE64:
    case NOR_CE1:
    case NOR_CE2:
        if (!s->wel) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mcxn-flexspi: erase without WREN is ignored\n");
            flexspi_done(s);
            return;
        }
        switch (cmd) {
        case NOR_SE:    nor_erase(s, addr, MCXN_NOR_SECTOR);  break;
        case NOR_BE32:  nor_erase(s, addr, MCXN_NOR_BLOCK32); break;
        case NOR_BE64:  nor_erase(s, addr, MCXN_NOR_BLOCK64); break;
        default:        nor_erase(s, 0, s->flash_size);       break;
        }
        s->wel = false;
        flexspi_done(s);
        return;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "mcxn-flexspi: unmodelled SPI-NOR opcode 0x%02x "
                      "(LUT seq %u)\n", cmd, seq);
        flexspi_error(s);
        return;
    }
}

static uint64_t mcxn_flexspi_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(opaque);
    uint32_t v = (off < MCXN_FLEXSPI_SIZE) ? s->regs[off >> 2] : 0;
    uint32_t avail, i;

    switch (off) {
    case FLEXSPI_MCR0:
        return v & ~MCR0_SWRESET;   /* SWRESET is momentary */
    case FLEXSPI_STS0:
        return STS0_IDLE;
    case FLEXSPI_STS1:
    case FLEXSPI_STS2:
    case FLEXSPI_AHBSPNDSTS:
        return 0;
    case FLEXSPI_IPRXFSTS:
        /* FILL is in 64-bit entries. */
        avail = s->rx_len - s->rx_pos;
        return (avail / 8) & 0xFF;
    case FLEXSPI_IPTXFSTS:
        return 0;
    default:
        if (off >= FLEXSPI_RFDR0 && off < FLEXSPI_RFDR0 + 32 * 4) {
            /* The guest reads RFDR[0..watermark] and then pops the FIFO by
             * clearing INTR[IPRXWA]; RFDR itself does not advance. */
            i = (off - FLEXSPI_RFDR0) / 4;
            uint32_t byte = s->rx_pos + i * 4;

            if (byte + 4 <= s->rx_len) {
                return ldl_le_p(&s->rx_buf[byte]);
            }
            if (byte < s->rx_len) {
                uint8_t tail[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

                memcpy(tail, &s->rx_buf[byte], s->rx_len - byte);
                return ldl_le_p(tail);
            }
            return 0xFFFFFFFFu;   /* past the data: an unprogrammed NOR reads 1s */
        }
        return v;
    }
}

static void mcxn_flexspi_write(void *opaque, hwaddr off, uint64_t value,
                               unsigned size)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(opaque);
    uint32_t val = value;
    uint32_t pop;

    if (off >= MCXN_FLEXSPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case FLEXSPI_STS0:
    case FLEXSPI_STS1:
    case FLEXSPI_STS2:
    case FLEXSPI_AHBSPNDSTS:
    case FLEXSPI_IPRXFSTS:
    case FLEXSPI_IPTXFSTS:
        return;   /* read-only status */

    case FLEXSPI_MCR0:
        s->regs[off >> 2] = val & ~MCR0_SWRESET;   /* SWRESET self-clears */
        return;

    case FLEXSPI_INTR:
        /* W1C.  Clearing IPRXWA pops a watermark's worth of RX data; clearing
         * IPTXWE pushes what the guest staged in TFDR. */
        if (val & INTR_IPRXWA) {
            pop = (FCR_WMRK(s->regs[FLEXSPI_IPRXFCR >> 2]) + 1) * 8;
            s->rx_pos = MIN(s->rx_pos + pop, s->rx_len);
        }
        s->regs[off >> 2] &= ~val;
        if (s->rx_pos < s->rx_len) {
            s->regs[off >> 2] |= INTR_IPRXWA;   /* more to drain */
        }
        if (s->pgm_pending && s->tx_len < s->pgm_len) {
            s->regs[off >> 2] |= INTR_IPTXWE;   /* more to feed */
        }
        mcxn_flexspi_update_irq(s);
        return;

    case FLEXSPI_INTEN:
        s->regs[off >> 2] = val;
        mcxn_flexspi_update_irq(s);
        return;

    case FLEXSPI_IPRXFCR:
        s->regs[off >> 2] = val & ~FCR_CLRF;
        if (val & FCR_CLRF) {
            s->rx_len = s->rx_pos = 0;
            s->regs[FLEXSPI_INTR >> 2] &= ~INTR_IPRXWA;
        }
        return;

    case FLEXSPI_IPTXFCR:
        s->regs[off >> 2] = val & ~FCR_CLRF;
        if (val & FCR_CLRF) {
            s->tx_len = 0;
        }
        return;

    case FLEXSPI_IPCMD:
        if (val & IPCMD_TRG) {
            flexspi_ip_command(s);
        }
        return;

    default:
        if (off >= FLEXSPI_RFDR0 && off < FLEXSPI_RFDR0 + 32 * 4) {
            return;   /* RX FIFO data is read-only */
        }
        if (off >= FLEXSPI_TFDR0 && off < FLEXSPI_TFDR0 + 32 * 4) {
            /* TX FIFO: the guest streams program data here.  Append in write
             * order — the SDK rewrites TFDR[0..watermark] each round. */
            if (s->pgm_pending && s->tx_len + 4 <= MCXN_FLEXSPI_XFER_MAX) {
                stl_le_p(&s->tx_buf[s->tx_len], val);
                s->tx_len += 4;
                if (s->tx_len >= s->pgm_len) {
                    flexspi_program_commit(s);
                }
            }
            return;
        }
        s->regs[off >> 2] = val;
        return;
    }
}

static const MemoryRegionOps mcxn_flexspi_ops = {
    .read = mcxn_flexspi_read,
    .write = mcxn_flexspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/*
 * A CPU store into the AHB NOR window.  On silicon this does not program the
 * flash — a NOR is written only by an erase + page-program sequence through the
 * controller — so it must not land here either.  (Backing this window with RAM
 * is what let firmware scribble at XIP addresses and appear to work.)
 */
static void mcxn_flexspi_nor_write(void *opaque, hwaddr off, uint64_t val,
                                   unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "mcxn-flexspi: store to the AHB NOR window at 0x%" HWADDR_PRIx
                  " is ignored — external NOR is not RAM.  Program it with the "
                  "FlexSPI IP command path (WREN + erase + page program).\n",
                  off);
}

static uint64_t mcxn_flexspi_nor_read(void *opaque, hwaddr off, unsigned size)
{
    /* Unreachable in romd mode: reads and instruction fetch go straight to the
     * backing array.  Present only because a ROM device must supply read ops. */
    return 0;
}

static const MemoryRegionOps mcxn_flexspi_nor_ops = {
    .read = mcxn_flexspi_nor_read,
    .write = mcxn_flexspi_nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mcxn_flexspi_reset(DeviceState *dev)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->wel = false;
    s->rx_len = s->rx_pos = s->tx_len = 0;
    s->pgm_pending = false;
    s->pgm_addr = s->pgm_len = 0;
}

static void mcxn_flexspi_realize(DeviceState *dev, Error **errp)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(dev);

    /* MMIO region 0: the CMSIS register file. */
    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_flexspi_ops, s,
                          TYPE_MCXN_FLEXSPI, MCXN_FLEXSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /*
     * MMIO region 1: the AHB-mapped external NOR, as a ROM *device*.  Reads and
     * instruction fetch go straight to the backing array — so XIP works and the
     * -kernel ROM loader can fill it — while stores are routed to the controller
     * and refused.  Never init_ram: that is what let guest stores program the
     * flash for free and kept the whole IP path a stub.
     */
    memory_region_init_rom_device(&s->nor, OBJECT(s), &mcxn_flexspi_nor_ops, s,
                                  "mcxn.flexspi-nor", s->flash_size, errp);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->nor);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property mcxn_flexspi_props[] = {
    /* Size of the AHB-mapped NOR window.  Default = the FRDM-MCXN947's
     * 8 MiB Winbond W25Q64 (Zephyr DTS: ranges @ 0x9000_0000, DT_SIZE_M(8)). */
    DEFINE_PROP_UINT64("flash-size", MCXNFlexSPIState, flash_size, 8 * MiB),
};

static const VMStateDescription vmstate_mcxn_flexspi = {
    .name = TYPE_MCXN_FLEXSPI,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNFlexSPIState, MCXN_FLEXSPI_SIZE / 4),
        VMSTATE_BOOL(wel, MCXNFlexSPIState),
        VMSTATE_UINT8_ARRAY(rx_buf, MCXNFlexSPIState, MCXN_FLEXSPI_XFER_MAX),
        VMSTATE_UINT32(rx_len, MCXNFlexSPIState),
        VMSTATE_UINT32(rx_pos, MCXNFlexSPIState),
        VMSTATE_UINT8_ARRAY(tx_buf, MCXNFlexSPIState, MCXN_FLEXSPI_XFER_MAX),
        VMSTATE_UINT32(tx_len, MCXNFlexSPIState),
        VMSTATE_BOOL(pgm_pending, MCXNFlexSPIState),
        VMSTATE_UINT32(pgm_addr, MCXNFlexSPIState),
        VMSTATE_UINT32(pgm_len, MCXNFlexSPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_flexspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_flexspi_realize;
    device_class_set_legacy_reset(dc, mcxn_flexspi_reset);
    device_class_set_props(dc, mcxn_flexspi_props);
    dc->vmsd = &vmstate_mcxn_flexspi;
}

static const TypeInfo mcxn_flexspi_types[] = {
    {
        .name          = TYPE_MCXN_FLEXSPI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNFlexSPIState),
        .class_init    = mcxn_flexspi_class_init,
    },
};

DEFINE_TYPES(mcxn_flexspi_types)
