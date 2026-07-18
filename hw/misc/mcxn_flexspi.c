/*
 * NXP MCX N FlexSPI — functional controller driving a real m25p80 SPI-NOR,
 * with an executable ROM-device mirror for XIP.  See header for the design and
 * the single-authority rule it rests on.
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
#include "hw/ssi/ssi.h"
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

/* MCR0 / INTR / IPCMD bits. */
#define MCR0_SWRESET   (1u << 0)
#define INTR_IPCMDDONE (1u << 0)
#define INTR_IPCMDERR  (1u << 3)
#define INTR_IPRXWA    (1u << 5)   /* RX FIFO has data for the guest    */
#define INTR_IPTXWE    (1u << 6)   /* TX FIFO wants data from the guest */
#define IPCMD_TRG      (1u << 0)

/*
 * STS0.  RM reset = 0x2: ARBIDLE (bit 1) set, SEQIDLE (bit 0) CLEAR -- because
 * MCR0[MDIS] is SET at reset and a DISABLED module's sequence engine is not "idle",
 * it is not running at all.  We hardwired 0x3 ("report the controller idle so the
 * pre-command wait passes"), which is a fabricated readiness of exactly the kind SCG
 * and VBAT were caught doing today: TELLING THE GUEST SOMETHING IT HAD NOT EARNED.
 * SEQIDLE now follows the module actually being enabled.
 */
#define STS0_ARBIDLE   0x2u
#define STS0_SEQIDLE   0x1u
#define MCR0_MDIS      0x2u        /* CMSIS FLEXSPI_MCR0_MDIS_MASK */

/* IPRXFCR / IPTXFCR. */
#define FCR_CLRF       (1u << 0)
#define FCR_DMAEN      (1u << 1)             /* IP{RX,TX}FCR[{RX,TX}DMAEN] */
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

#define LUT_STOP   0x00
#define LUT_CMD    0x01
#define LUT_RADDR  0x02
#define LUT_WRITE  0x08
#define LUT_READ   0x09
#define LUT_DUMMY  0x0C
#define LUT_DDR    0x20   /* DDR variants are the SDR opcode | 0x20 */

/* SPI-NOR opcodes we must recognise to keep the XIP mirror in step. */
#define NOR_WREN        0x06
#define NOR_READ        0x03
#define NOR_PP          0x02
#define NOR_QPP         0x32
#define NOR_SE          0x20   /* 4 KiB sector erase */
#define NOR_BE32        0x52
#define NOR_BE64        0xD8
#define NOR_CE1         0xC7
#define NOR_CE2         0x60

/* A decoded LUT sequence: the SPI transaction it describes. */
typedef struct {
    bool    valid;
    uint8_t cmd;
    int     addr_bits;
    int     dummy_cycles;
    bool    has_read;
    bool    has_write;
} FlexSPISeq;

static uint8_t *mirror_ptr(MCXNFlexSPIState *s)
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

/* --- raw SPI: the controller talks to m25p80 exactly as silicon would ------ */

static void spi_select(MCXNFlexSPIState *s, bool on)
{
    qemu_set_irq(s->cs, on ? 0 : 1);      /* CS is active low */
}

static void spi_send_addr(MCXNFlexSPIState *s, uint32_t addr, int bits)
{
    for (int i = bits / 8 - 1; i >= 0; i--) {
        ssi_transfer(s->spi, (addr >> (i * 8)) & 0xFF);
    }
}

/* Read len bytes out of the flash itself (opcode 0x03 + 24-bit address). */
static void nor_read(MCXNFlexSPIState *s, uint32_t off, uint8_t *buf,
                     uint32_t len)
{
    spi_select(s, true);
    ssi_transfer(s->spi, NOR_READ);
    spi_send_addr(s, off, 24);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = ssi_transfer(s->spi, 0);
    }
    spi_select(s, false);
}

/*
 * Re-derive the XIP mirror from the flash over [off, off+len), then publish it
 * so TCG drops translation blocks for code that was just reprogrammed.  The
 * mirror is never written any other way: m25p80 is the only authority, so the
 * two cannot drift apart.
 */
static void mirror_resync(MCXNFlexSPIState *s, uint32_t off, uint32_t len)
{
    if (off >= s->flash_size) {
        return;
    }
    len = MIN(len, (uint32_t)(s->flash_size - off));
    nor_read(s, off, mirror_ptr(s) + off, len);
    memory_region_flush_rom_device(&s->nor, off, len);
}

/* Program one page into the flash.  WREN first: m25p80 enforces the latch. */
static void nor_program_page(MCXNFlexSPIState *s, uint32_t off,
                             const uint8_t *data, uint32_t len)
{
    spi_select(s, true);
    ssi_transfer(s->spi, NOR_WREN);
    spi_select(s, false);

    spi_select(s, true);
    ssi_transfer(s->spi, NOR_PP);
    spi_send_addr(s, off, 24);
    for (uint32_t i = 0; i < len; i++) {
        ssi_transfer(s->spi, data[i]);
    }
    spi_select(s, false);
}

/*
 * An image linked into the XIP window is written straight into the mirror by
 * QEMU's ROM loader (address_space_write_rom bypasses our write op), so the NOR
 * itself has never seen it.  Program it in before the flash is first consulted —
 * on hardware the firmware IS in the flash.
 *
 * Leaving it mirror-only is the trap: the two would disagree, and the first
 * erase would silently resurrect stale content underneath a running image — a
 * brand-new silent-wrong created by the fix for one.  Only non-blank pages are
 * programmed, so a board with no XIP image costs nothing.
 */
static void flexspi_flush_loader_image(MCXNFlexSPIState *s)
{
    const uint8_t *m = mirror_ptr(s);

    s->loader_flushed = true;

    for (uint64_t off = 0; off + MCXN_NOR_PAGE <= s->flash_size;
         off += MCXN_NOR_PAGE) {
        bool blank = true;

        for (uint32_t i = 0; i < MCXN_NOR_PAGE; i++) {
            if (m[off + i] != 0xFF) {
                blank = false;
                break;
            }
        }
        if (!blank) {
            nor_program_page(s, off, m + off, MCXN_NOR_PAGE);
        }
    }
}

/* --- LUT ------------------------------------------------------------------- */

static void seq_add(FlexSPISeq *q, uint32_t op, uint32_t operand)
{
    switch (op & ~LUT_DDR) {
    case LUT_CMD:   q->cmd = operand; q->valid = true; break;
    case LUT_RADDR: q->addr_bits = operand;            break;
    case LUT_DUMMY: q->dummy_cycles = operand;         break;
    case LUT_READ:  q->has_read = true;                break;
    case LUT_WRITE: q->has_write = true;               break;
    default: break;
    }
}

/* Decode the sequence selected by IPCR1[ISEQID] into the transaction it means. */
static FlexSPISeq flexspi_decode_lut(MCXNFlexSPIState *s, uint32_t seq)
{
    FlexSPISeq q = { 0 };

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t lut = s->regs[(FLEXSPI_LUT0 >> 2) + seq * 4 + i];
        uint32_t op0 = LUT_OPCODE0(lut), op1 = LUT_OPCODE1(lut);

        if ((op0 & ~LUT_DDR) == LUT_STOP) {
            return q;
        }
        seq_add(&q, op0, LUT_OPERAND0(lut));

        if ((op1 & ~LUT_DDR) == LUT_STOP) {
            return q;
        }
        seq_add(&q, op1, LUT_OPERAND1(lut));
    }
    return q;
}

/* How much of the mirror a modifying opcode invalidates. */
static bool flexspi_dirty_range(uint8_t cmd, uint32_t addr, uint64_t size,
                                uint32_t *off, uint32_t *len)
{
    switch (cmd) {
    case NOR_PP:
    case NOR_QPP:
        *off = addr & ~(uint32_t)(MCXN_NOR_PAGE - 1);
        *len = MCXN_NOR_PAGE;
        return true;
    case NOR_SE:
        *off = addr & ~(uint32_t)(MCXN_NOR_SECTOR - 1);
        *len = MCXN_NOR_SECTOR;
        return true;
    case NOR_BE32:
        *off = addr & ~(uint32_t)(MCXN_NOR_BLOCK32 - 1);
        *len = MCXN_NOR_BLOCK32;
        return true;
    case NOR_BE64:
        *off = addr & ~(uint32_t)(MCXN_NOR_BLOCK64 - 1);
        *len = MCXN_NOR_BLOCK64;
        return true;
    case NOR_CE1:
    case NOR_CE2:
        *off = 0;
        *len = size;
        return true;
    default:
        return false;
    }
}

/* Finish a transaction: drop CS, refresh whatever it changed, complete. */
static void flexspi_finish(MCXNFlexSPIState *s, uint8_t cmd, uint32_t addr)
{
    uint32_t doff, dlen;

    spi_select(s, false);
    if (flexspi_dirty_range(cmd, addr, s->flash_size, &doff, &dlen)) {
        mirror_resync(s, doff, dlen);
    }
    s->pgm_pending = false;
    s->regs[FLEXSPI_INTR >> 2] &= ~INTR_IPTXWE;
    flexspi_done(s);
}

/* IPCMD[TRG]: run the LUT sequence against the flash. */
static void flexspi_update_dma_req(MCXNFlexSPIState *s);

static void flexspi_ip_command(MCXNFlexSPIState *s)
{
    uint32_t ipcr1 = s->regs[FLEXSPI_IPCR1 >> 2];
    uint32_t datsz = IPCR1_IDATSZ(ipcr1);
    uint32_t addr  = nor_off(s, s->regs[FLEXSPI_IPCR0 >> 2]);
    FlexSPISeq q = flexspi_decode_lut(s, IPCR1_ISEQID(ipcr1));

    s->rx_len = s->rx_pos = s->tx_len = 0;
    s->regs[FLEXSPI_INTR >> 2] &= ~(INTR_IPRXWA | INTR_IPTXWE);

    if (!q.valid) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mcxn-flexspi: IP command with no CMD in LUT seq %u\n",
                      IPCR1_ISEQID(ipcr1));
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

    /* The XIP image must be in the flash before the flash is ever consulted. */
    if (!s->loader_flushed) {
        flexspi_flush_loader_image(s);
    }

    spi_select(s, true);
    ssi_transfer(s->spi, q.cmd);
    if (q.addr_bits) {
        spi_send_addr(s, addr, q.addr_bits);
    }
    for (int i = 0; i < q.dummy_cycles / 8; i++) {
        ssi_transfer(s->spi, 0);
    }

    if (q.has_read) {
        for (uint32_t i = 0; i < datsz; i++) {
            s->rx_buf[i] = ssi_transfer(s->spi, 0);
        }
        s->rx_len = datsz;
        if (datsz) {
            s->regs[FLEXSPI_INTR >> 2] |= INTR_IPRXWA;
        }
        flexspi_finish(s, q.cmd, addr);
        flexspi_update_dma_req(s);   /* RX FIFO now has data -- ask the eDMA */
        return;
    }

    if (q.has_write && datsz) {
        /*
         * The SDK triggers the command and only THEN feeds TFDR, so the SPI
         * transaction has to stay open: CS stays asserted while the guest
         * streams the program data, and the mirror is refreshed on the last byte.
         */
        s->pgm_pending = true;
        s->pgm_addr = addr;
        s->pgm_len = datsz;
        s->regs[FLEXSPI_INTR >> 2] |= INTR_IPTXWE;
        mcxn_flexspi_update_irq(s);
        flexspi_update_dma_req(s);   /* TX FIFO wants program data -- ask the eDMA */
        return;
    }

    /* No data phase: WREN/WRDI, erase, and friends. */
    flexspi_finish(s, q.cmd, addr);
}

/*
 * Drive the RX/TX eDMA request lines.  RX asks while the read FIFO still holds data (and
 * IPRXFCR[RXDMAEN] is set); TX asks while the controller wants program data (INTR[IPTXWE]
 * and IPTXFCR[TXDMAEN]).  In DMA mode RFDR0 auto-pops per read (below), so the engine
 * drains the FIFO word by word until the request drops -- without this the DMA never runs.
 */
static void flexspi_update_dma_req(MCXNFlexSPIState *s)
{
    bool rx = (s->rx_pos < s->rx_len) &&
              (s->regs[FLEXSPI_IPRXFCR >> 2] & FCR_DMAEN);
    bool tx = (s->regs[FLEXSPI_INTR >> 2] & INTR_IPTXWE) &&
              (s->regs[FLEXSPI_IPTXFCR >> 2] & FCR_DMAEN);

    if (rx != s->rx_dma_lvl) {
        s->rx_dma_lvl = rx;
        qemu_set_irq(s->dma_req_rx, rx);
    }
    if (tx != s->tx_dma_lvl) {
        s->tx_dma_lvl = tx;
        qemu_set_irq(s->dma_req_tx, tx);
    }
}

static uint64_t mcxn_flexspi_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(opaque);
    uint32_t v = (off < MCXN_FLEXSPI_SIZE) ? s->regs[off >> 2] : 0;
    uint32_t avail, i, byte;

    switch (off) {
    case FLEXSPI_MCR0:
        return v & ~MCR0_SWRESET;   /* SWRESET is momentary */
    case FLEXSPI_STS0:
        /* ARBIDLE always; SEQIDLE only once the module is actually ENABLED (MDIS
         * clear).  A disabled sequence engine is not "idle" -- it is not running. */
        return STS0_ARBIDLE |
               ((s->regs[FLEXSPI_MCR0 / 4] & MCR0_MDIS) ? 0 : STS0_SEQIDLE);
    case FLEXSPI_STS2:
        /*
         * ⚠ A HARDCODED `return 0` IN A READ PATH IS A CLAIM, AND IT SILENTLY BEAT THE
         *   RESET TABLE: the value was seeded into regs[] and this line never looked.
         *
         *   STS2 is the DLL status.  RM reset 0x0100_0100 -- the AREFSEL/BREFSEL delay-
         *   line taps at their power-on positions.  (⚠ AND I ALMOST CLAIMED THIS MEANT
         *   "a driver waiting for DLL lock spins forever".  IT DOES NOT: ASLVLOCK and
         *   AREFLOCK are 0 in the RM's reset TOO.  The bug is real and MUNDANE, and
         *   reaching for the scariest reading is how you ship a scary story instead of
         *   a fixed model.  CHECK THE FIELD BEFORE YOU CLAIM THE CONSEQUENCE.)
         */
        return s->regs[FLEXSPI_STS2 / 4];
    case FLEXSPI_STS1:
    case FLEXSPI_AHBSPNDSTS:
    case FLEXSPI_IPTXFSTS:
        return 0;
    case FLEXSPI_IPRXFSTS:
        /*
         * FILL counts 64-bit FIFO entries and must ROUND UP: a partially filled
         * entry still holds readable bytes.  The stock SDK's small-read path
         * spins on `size > FILL * 8` (fsl_flexspi.c, FLEXSPI_ReadBlocking), so
         * rounding down reports FILL = 0 for anything under 8 bytes and the real
         * driver hangs forever on a 3-byte JEDEC ID.  Model what the driver
         * polls, not just what the RM lists.  (Found by rt1180emulator.)
         */
        avail = s->rx_len - s->rx_pos;
        return ((avail + 7) / 8) & 0xFF;
    default:
        if (off >= FLEXSPI_RFDR0 && off < FLEXSPI_RFDR0 + 32 * 4) {
            /* In interrupt mode the guest reads RFDR[0..watermark] and pops the FIFO
             * by clearing INTR[IPRXWA]; RFDR itself does not advance. */
            uint32_t ret;

            i = (off - FLEXSPI_RFDR0) / 4;
            byte = s->rx_pos + i * 4;

            if (byte + 4 <= s->rx_len) {
                ret = ldl_le_p(&s->rx_buf[byte]);
            } else if (byte < s->rx_len) {
                uint8_t tail[4] = { 0xFF, 0xFF, 0xFF, 0xFF };

                memcpy(tail, &s->rx_buf[byte], s->rx_len - byte);
                ret = ldl_le_p(tail);
            } else {
                ret = 0xFFFFFFFFu;   /* past the data: an erased NOR reads ones */
            }

            /* In DMA mode there is no separate IACK for the eDMA to issue, so RFDR0
             * pops on each read: advance one word, drop IPRXWA when the FIFO empties,
             * and re-evaluate the request line so the transfer terminates on its own. */
            if (i == 0 && (s->regs[FLEXSPI_IPRXFCR >> 2] & FCR_DMAEN) &&
                s->rx_pos < s->rx_len) {
                s->rx_pos += 4;
                if (s->rx_pos >= s->rx_len) {
                    s->regs[FLEXSPI_INTR >> 2] &= ~INTR_IPRXWA;
                    mcxn_flexspi_update_irq(s);
                }
                flexspi_update_dma_req(s);
            }
            return ret;
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
        /* W1C.  Clearing IPRXWA pops a watermark's worth of RX data. */
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
        flexspi_update_dma_req(s);
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
        flexspi_update_dma_req(s);   /* RXDMAEN may have just been armed/cleared */
        return;

    case FLEXSPI_IPTXFCR:
        s->regs[off >> 2] = val & ~FCR_CLRF;
        if ((val & FCR_CLRF) && !s->pgm_pending) {
            s->tx_len = 0;
        }
        flexspi_update_dma_req(s);   /* TXDMAEN may have just been armed/cleared */
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
            /*
             * TX FIFO: the guest streams program data here while CS is still
             * asserted, so the bytes go straight down the SPI bus to the flash.
             * Append in write order — the SDK rewrites TFDR[0..watermark] each
             * round.
             */
            if (s->pgm_pending) {
                for (int i = 0; i < 4 && s->tx_len < s->pgm_len; i++) {
                    ssi_transfer(s->spi, (val >> (i * 8)) & 0xFF);
                    s->tx_len++;
                }
                if (s->tx_len >= s->pgm_len) {
                    flexspi_finish(s, NOR_PP, s->pgm_addr);
                    s->regs[FLEXSPI_INTR >> 2] &= ~INTR_IPTXWE;   /* program complete */
                    mcxn_flexspi_update_irq(s);
                }
                /* Re-evaluate the TX request: it stays asserted while the controller
                 * still wants data, and drops when tx_len fills -- so a DMA feeding
                 * TFDR terminates on its own instead of overrunning the FIFO. */
                flexspi_update_dma_req(s);
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
 * controller — so it must not land here either.  It must also not touch the
 * mirror, which is derived from the flash and from nothing else.
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
     * mirror.  Present only because a ROM device must supply read ops. */
    return 0;
}

static const MemoryRegionOps mcxn_flexspi_nor_ops = {
    .read = mcxn_flexspi_nor_read,
    .write = mcxn_flexspi_nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

/*
 * FLEXSPI0 reset values, from the RM's register map (generated by
 * tests/mcxn-reset-values/extract-rm-golden.py -- derived, never invented).
 */
static const struct { uint16_t off; uint32_t val; } rst_flexspi0[] = {
    { 0x000, 0xFFFF8002u },   /* MCR0 */
    { 0x004, 0xFFFFFFFFu },   /* MCR1 */
    { 0x008, 0x200081F7u },   /* MCR2 */
    { 0x00C, 0x00000018u },   /* AHBCR */
    { 0x018, 0x5AF05AF0u },   /* LUTKEY */
    { 0x01C, 0x00000002u },   /* LUTCR */
    { 0x094, 0x000000C3u },   /* FLSHCR4 */
    { 0x0E0, 0x00000002u },   /* STS0 */
    { 0x0E8, 0x01000100u },   /* STS2 */
    { 0x500, 0x55555555u },   /* IPEDCTXCTRL0 */
    { 0x504, 0xAAAAAAAAu },   /* IPEDCTXCTRL1 */
};

static void mcxn_flexspi_reset(DeviceState *dev)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(dev);
    int rst_i;

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x0E8 / 4] = 0x01000100u;   /* STS2 -- RM reset, derived */
    for (rst_i = 0; rst_i < (int)ARRAY_SIZE(rst_flexspi0); rst_i++) {
        s->regs[rst_flexspi0[rst_i].off / 4] = rst_flexspi0[rst_i].val;
    }
    s->rx_len = s->rx_pos = s->tx_len = 0;
    s->pgm_pending = false;
    s->pgm_addr = s->pgm_len = 0;
    s->rx_dma_lvl = s->tx_dma_lvl = false;
    qemu_set_irq(s->dma_req_rx, 0);
    qemu_set_irq(s->dma_req_tx, 0);
    /*
     * loader_flushed is deliberately NOT cleared.  The ROM loader refills the
     * mirror on every reset, and re-flushing it into the NOR would overwrite
     * whatever the guest had legitimately programmed there before the reset.
     */
}

static void mcxn_flexspi_realize(DeviceState *dev, Error **errp)
{
    MCXNFlexSPIState *s = MCXN_FLEXSPI(dev);

    /* MMIO region 0: the CMSIS register file. */
    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_flexspi_ops, s,
                          TYPE_MCXN_FLEXSPI, MCXN_FLEXSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    /*
     * MMIO region 1: the AHB XIP window, a ROM *device* mirroring the flash.
     * Reads and instruction fetch are direct (XIP stays TCG-cacheable, and the
     * -kernel ROM loader can fill it); stores are routed away and refused.
     */
    memory_region_init_rom_device(&s->nor, OBJECT(s), &mcxn_flexspi_nor_ops, s,
                                  "mcxn.flexspi-nor", s->flash_size, errp);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->nor);

    /* The real flash lives on our SSI bus; the SoC attaches m25p80 to it. */
    s->spi = ssi_create_bus(dev, "flexspi");
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->cs);       /* sysbus IRQ 0: chip select   */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);      /* sysbus IRQ 1: NVIC line      */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req_rx); /* sysbus IRQ 2: eDMA src 1   */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req_tx); /* sysbus IRQ 3: eDMA src 2   */
}

static const Property mcxn_flexspi_props[] = {
    /* Size of the AHB-mapped NOR window.  Default = the FRDM-MCXN947's
     * 8 MiB Winbond W25Q64 (Zephyr DTS: ranges @ 0x9000_0000, DT_SIZE_M(8)). */
    DEFINE_PROP_UINT64("flash-size", MCXNFlexSPIState, flash_size, 8 * MiB),
};

static const VMStateDescription vmstate_mcxn_flexspi = {
    .name = TYPE_MCXN_FLEXSPI,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNFlexSPIState, MCXN_FLEXSPI_SIZE / 4),
        VMSTATE_BOOL(loader_flushed, MCXNFlexSPIState),
        VMSTATE_UINT8_ARRAY(rx_buf, MCXNFlexSPIState, MCXN_FLEXSPI_XFER_MAX),
        VMSTATE_UINT32(rx_len, MCXNFlexSPIState),
        VMSTATE_UINT32(rx_pos, MCXNFlexSPIState),
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
