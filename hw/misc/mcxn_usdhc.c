/*
 * NXP MCX N uSDHC (Ultra Secured Digital Host Controller) — real SD data
 * path.
 *
 * This block used to CONJURE ITS OWN CARD, and the capability table claimed
 * "uSDHC ADMA block data" for it.  Both halves of that were false:
 *
 *   - There was no ADMA and no block data.  No dma_memory_read/write, no
 *     address_space, no descriptor walk, no storage: ADMA existed only as two
 *     register offsets.  The model could not move a single byte.
 *   - Card responses for CMD8/CMD3/ACMD41/CMD2/CMD9 were INVENTED in
 *     CMD_RSP0..3, and PRES_STATE[CINST] was hardwired so firmware "detected" a
 *     card that was not there.
 *
 * ⭐ WHY THE MUTATION AUDIT MISSED IT — worth remembering, because it is
 * the one
 * blind spot the technique has.  The old test DID read data back and compare it
 * to an expected value, so it looked properly guarded: mutate CMD_RSP0 and it
 * duly failed.  But CMD8 only "passed" because the model ECHOED THE TEST'S OWN
 * ARGUMENT back (0x1AA in, 0x1AA out), and CMD3 only "passed" because the test
 * had been told the model's hardcoded RCA.  Both sides of the comparison came
 * out of the same fiction — the model was its own oracle.  Mutation testing
 * proves a test is COUPLED to the model; it does NOT prove the test checks
 * anything REAL, and against a conjured peer it is blind by construction.  The
 * golden has to live somewhere the model cannot reach.
 *
 * So this model does what the silicon does and NOTHING MORE: it drives a real
 * SD bus.  It supplies the BUS; it does not invent a card onto it.  The
 * board or
 * the operator attaches a genuine QEMU sd-card:
 *
 *     -device sd-card,drive=<id>   (on the "sd-bus" this device creates)
 *
 * and every response and every byte then comes from that card — the same
 * discipline FlexSPI now uses with a real m25p80 and I3C with a real at24c.
 *
 * What is real here:
 *   - commands go out over sdbus_do_command() and the response is marshalled
 *     into CMD_RSP0..3 exactly as QEMU's own sdhci.c does it (R2's 128-bit
 *     layout is fiddly and is copied, not guessed);
 *   - WITH NO CARD INSERTED, COMMANDS TIME OUT (INT_STATUS[CTOE]) rather than
 *     returning a plausible answer.  This is the I3C-NACK lesson applied to
 *     storage: firmware must be able to tell "no card" from "a card said yes",
 *     and a host that always answers is lying about a slot that is empty.  CTOE
 *     is a real, NON-GATING error channel — the driver is informed, not hung;
 *   - PRES_STATE[CINST] reflects sdbus_get_inserted(), i.e. the truth;
 *   - the data phase really moves bytes: a real ADMA2 descriptor walk
 *     (VALID/END/LINK/TRAN honoured, bad descriptors raise INT_STATUS[DMAE]),
 *     simple SDMA via DS_ADDR, and PIO through DATA_BUFF_ACC_PORT.
 *
 * Offsets and bit masks come from the MCXN947 CMSIS header (USDHC_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/misc/mcxn_usdhc.h"
#include "hw/core/irq.h"
#include "system/dma.h"
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
#define CMD_XFR_CMDINX_SHIFT 24
#define CMD_XFR_CMDINX_MASK  0x3Fu

/* INT_STATUS bits (CMSIS USDHC_INT_STATUS_*). */
#define INT_CC   (1u << 0)    /* command complete       */
#define INT_TC   (1u << 1)    /* transfer complete      */
#define INT_CTOE (1u << 16)   /* command timeout error  */
#define INT_DTOE (1u << 20)   /* data timeout error     */
#define INT_DMAE (1u << 28)   /* DMA error              */

/*
 * MIX_CTRL bits (CMSIS USDHC_MIX_CTRL_*) — uSDHC keeps the transfer-type bits
 * here, not in CMD_XFR_TYP as stock SDHCI does.
 */
#define MIX_DMAEN   (1u << 0)
#define MIX_BCEN    (1u << 1)
#define MIX_DTDSEL  (1u << 4)   /* 1 = read (card -> host) */

/* PROT_CTRL[DMASEL] (CMSIS USDHC_PROT_CTRL_DMASEL_MASK = 0x300). */
#define PROT_DMASEL_MASK   0x300u
#define PROT_DMASEL_SHIFT  8
#define DMASEL_ADMA2       2u

/* BLK_ATT (CMSIS USDHC_BLK_ATT_*). */
#define BLK_BLKSIZE_MASK  0x1FFFu
#define BLK_BLKCNT_SHIFT  16

/*
 * ADMA2 32-bit descriptor attributes (Host Controller spec; same encoding as
 * QEMU's own sdhci.c, from which the layout is taken rather than guessed).
 */
#define ADMA_VALID     (1u << 0)
#define ADMA_END       (1u << 1)
#define ADMA_ACT_MASK  (3u << 4)
#define ADMA_ACT_TRAN  (2u << 4)
#define ADMA_ACT_LINK  (3u << 4)

/*
 * HOST_CTRL_CAP: the part telling software WHAT IT CAN DO.  RM reset
 * 0x07F3_B407.
 *
 * ⚠ I HAD THIS EXACTLY BACKWARDS, AND I WROTE THE BACKWARDS REASONING DOWN.
 *
 *   The old comment argued: "we return 0x07F3_0000 with the low half zeroed;
 *   those bits
 *   are SDR50/SDR104/DDR50 and the tuning fields.  A CAPABILITY REGISTER THAT
 *   UNDER-REPORTS IS A PART LYING ABOUT WHAT IT IS."  So I "fixed" it
 *   TO THE RM VALUE.
 *
 *   ⭐ THAT IS THE TRAP, AND IT IS THE ONE 91emulator PAID FOR WITH
 *   A BOOT FAILURE.
 *     Their gate said the same thing about the same register; they set it
 *     to the RM
 *     value; and QEMU's OWN sdhci_check_capareg() REFUSED TO START --
 *     because the value
 *     advertises hardware the model does not implement.  TWO ORACLES
 *     DISAGREED AND THE
 *     ONE THAT REFUSED TO BOOT WAS RIGHT.  My uSDHC is a private model:
 *     THERE IS NO SUCH
 *     ASSERTION HERE TO CATCH ME.
 *
 *   ⭐ ON A CAPABILITY REGISTER, MATCHING THE REFERENCE MANUAL IS THE BUG --
 *   unless you
 *     also implement the chip behind it.  UNDER-REPORTING is a model
 *     that promises less
 *     than the silicon.  OVER-REPORTING IS A PROMISE THE EMULATOR MAKES
 *     ON THE CHIP'S
 *     BEHALF, AND THE GUEST WILL HOLD US TO IT.
 *
 * WHAT WE ACTUALLY IMPLEMENT: ADMA2 against a REAL sd-card, high speed,
 * the standard
 * voltages and bus widths.  THERE IS NO TUNING ENGINE AT ALL -- no CMD19,
 * no sampling
 * window, no SDR104/DDR50 path; `USDHC_TUNING_CTRL` is a storage location
 * and nothing
 * more.  A driver that saw SDR104 would switch the card to it and then
 * run the tuning
 * procedure into a model that has none.
 *
 * So we clear EXACTLY the UHS-I capability bits and NOTHING ELSE --
 * ADMAS/DMAS/HSS/SRS,
 * the voltages (VS33/VS30/VS18) and the max block length stay as the RM
 * gives them,
 * because those we DO deliver.
 *
 * ⇒ DECISION, not a gap.  The silicon has these modes; this model
 *   does not, yet.  When
 *   the tuning engine lands, these bits come back WITH it -- and not
 *   one moment before.
 */
#define CAP_UHS_I_UNIMPLEMENTED  \
    (0x1u    /* SDR50_SUPPORT    */ | \
     0x2u    /* SDR104_SUPPORT   */ | \
     0x4u    /* DDR50_SUPPORT    */ | \
     0x2000u /* USE_TUNING_SDR50 */)

#define HOST_CTRL_CAP_VALUE  (0x07F3B407u & ~CAP_UHS_I_UNIMPLEMENTED)

static void mcxn_usdhc_update_irq(MCXNUSDHCState *s)
{
    uint32_t active = s->regs[USDHC_INT_STATUS >> 2] &
                      s->regs[USDHC_INT_SIGNAL_EN >> 2];
    qemu_set_irq(s->irq, active != 0);
}

/*
 * Move the data phase of the command just issued.  Bytes come from, or go to,
 * the REAL card on the bus — there is no internal buffer for them to be
 * invented in.
 */
static void mcxn_usdhc_do_adma2(MCXNUSDHCState *s, bool read, uint64_t total)
{
    hwaddr desc = s->regs[USDHC_ADMA_SYS_ADDR >> 2];
    uint64_t moved = 0;
    uint8_t buf[512];
    int guard = 0;

    while (moved < total) {
        uint64_t raw;
        hwaddr addr;
        uint32_t len;
        uint8_t attr;

        /* A runaway LINK chain must not wedge the machine. */
        if (++guard > 4096) {
            s->regs[USDHC_INT_STATUS >> 2] |= INT_DMAE;
            return;
        }
        if (dma_memory_read(&address_space_memory, desc, &raw, sizeof(raw),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            s->regs[USDHC_INT_STATUS >> 2] |= INT_DMAE;
            return;
        }
        raw  = le64_to_cpu(raw);
        attr = raw & 0x7F;
        len  = extract64(raw, 16, 16);
        addr = extract64(raw, 32, 32) & ~0x3ull;

        if (!(attr & ADMA_VALID)) {
            /* An invalid descriptor is a real error, not something to skip. */
            s->regs[USDHC_INT_STATUS >> 2] |= INT_DMAE;
            return;
        }

        switch (attr & ADMA_ACT_MASK) {
        case ADMA_ACT_TRAN:
            if (len == 0) {
                len = 65536;         /* a zero length field means 64 KiB */
            }
            while (len > 0 && moved < total) {
                uint32_t n = MIN(len, sizeof(buf));

                n = MIN(n, total - moved);
                if (read) {
                    sdbus_read_data(&s->sdbus, buf, n);
                    dma_memory_write(&address_space_memory, addr, buf, n,
                                     MEMTXATTRS_UNSPECIFIED);
                } else {
                    dma_memory_read(&address_space_memory, addr, buf, n,
                                    MEMTXATTRS_UNSPECIFIED);
                    sdbus_write_data(&s->sdbus, buf, n);
                }
                addr  += n;
                len   -= n;
                moved += n;
            }
            break;
        case ADMA_ACT_LINK:
            desc = addr;
            continue;                /* LINK does not advance past itself */
        default:
            break;                   /* NOP / SET_LEN: nothing to move */
        }

        if (attr & ADMA_END) {
            break;
        }
        desc += 8;
    }

    s->regs[USDHC_ADMA_SYS_ADDR >> 2] = desc;
}

static void mcxn_usdhc_do_data(MCXNUSDHCState *s)
{
    uint32_t blk     = s->regs[USDHC_BLK_ATT >> 2];
    uint32_t mix     = s->regs[USDHC_MIX_CTRL >> 2];
    uint32_t blksize = blk & BLK_BLKSIZE_MASK;
    uint32_t blkcnt  = (blk >> BLK_BLKCNT_SHIFT) & 0xFFFF;
    bool read        = mix & MIX_DTDSEL;
    uint64_t total;

    if (!(mix & MIX_BCEN) || blkcnt == 0) {
        blkcnt = 1;                  /* block count disabled: a single block */
    }
    total = (uint64_t)blksize * blkcnt;
    if (total == 0) {
        s->regs[USDHC_INT_STATUS >> 2] |= INT_TC;
        return;
    }

    if (!(mix & MIX_DMAEN)) {
        /* PIO: firmware will move the bytes itself through the buffer port. */
        s->data_len   = total;
        s->data_pos   = 0;
        s->data_read  = read;
        return;
    }

    if (((s->regs[USDHC_PROT_CTRL >> 2] & PROT_DMASEL_MASK) >>
         PROT_DMASEL_SHIFT) == DMASEL_ADMA2) {
        mcxn_usdhc_do_adma2(s, read, total);
    } else {
        /* Simple (SDMA) contiguous transfer from DS_ADDR. */
        hwaddr addr = s->regs[USDHC_DS_ADDR >> 2];
        uint64_t moved = 0;
        uint8_t buf[512];

        while (moved < total) {
            uint32_t n = MIN(sizeof(buf), total - moved);

            if (read) {
                sdbus_read_data(&s->sdbus, buf, n);
                dma_memory_write(&address_space_memory, addr, buf, n,
                                 MEMTXATTRS_UNSPECIFIED);
            } else {
                dma_memory_read(&address_space_memory, addr, buf, n,
                                MEMTXATTRS_UNSPECIFIED);
                sdbus_write_data(&s->sdbus, buf, n);
            }
            addr  += n;
            moved += n;
        }
        s->regs[USDHC_DS_ADDR >> 2] = addr;
    }

    s->regs[USDHC_INT_STATUS >> 2] |= INT_TC;
}

/*
 * Issue a command to the REAL card on the bus and marshal its response.
 *
 * With no card inserted, sdbus_do_command() returns 0 and we raise CTOE — a
 * command timeout, which is what the silicon does to an empty slot.  Firmware
 * MUST be able to distinguish "there is no card" from "the card answered".
 */
static void mcxn_usdhc_send_command(MCXNUSDHCState *s, uint32_t xfrtyp)
{
    uint8_t response[16];
    SDRequest request;
    size_t rlen;

    request.cmd = (xfrtyp >> CMD_XFR_CMDINX_SHIFT) & CMD_XFR_CMDINX_MASK;
    request.arg = s->regs[USDHC_CMD_ARG >> 2];

    rlen = sdbus_do_command(&s->sdbus, &request, response, sizeof(response));

    if (rlen == 4) {
        s->regs[USDHC_CMD_RSP0 >> 2] = ldl_be_p(response);
        s->regs[USDHC_CMD_RSP1 >> 2] = 0;
        s->regs[USDHC_CMD_RSP2 >> 2] = 0;
        s->regs[USDHC_CMD_RSP3 >> 2] = 0;
    } else if (rlen == 16) {
        /*
         * R2: 128-bit CID/CSD.  Layout copied from QEMU's sdhci.c — the
         * shift by one byte is real and easy to get wrong.
         */
        s->regs[USDHC_CMD_RSP0 >> 2] = ldl_be_p(&response[11]);
        s->regs[USDHC_CMD_RSP1 >> 2] = ldl_be_p(&response[7]);
        s->regs[USDHC_CMD_RSP2 >> 2] = ldl_be_p(&response[3]);
        s->regs[USDHC_CMD_RSP3 >> 2] = (response[0] << 16) |
                                       (response[1] << 8) | response[2];
    } else {
        /*
         * No card, or the card refused the command: TIME OUT.  Do not answer
         * on its behalf, and do not hang — CTOE is non-gating and the driver
         * checks it alongside CC.
         */
        s->regs[USDHC_INT_STATUS >> 2] |= INT_CTOE;
        mcxn_usdhc_update_irq(s);
        return;
    }

    s->regs[USDHC_INT_STATUS >> 2] |= INT_CC;

    if (xfrtyp & CMD_XFR_DPSEL) {
        mcxn_usdhc_do_data(s);
    }
    mcxn_usdhc_update_irq(s);
}

static uint64_t mcxn_usdhc_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNUSDHCState *s = MCXN_USDHC(opaque);
    uint32_t v = (off < MCXN_USDHC_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case USDHC_PRES_STATE: {
        /*
         * Lines are idle (commands complete synchronously) and the SD clock
         * reads stable.  CINST is the TRUTH, from the bus: an empty slot must
         * not report a card.  This used to be hardwired, so firmware
         * "detected" a card that did not exist.
         */
        uint32_t v2 = PRES_SDSTB;

        if (sdbus_get_inserted(&s->sdbus)) {
            v2 |= PRES_CINST;
        }
        return v2;
    }
    case USDHC_DATA_BUFF_ACC_PORT: {
        /* PIO read: the bytes come from the card, not from a backing store. */
        uint32_t w = 0;

        if (s->data_len == 0 || !s->data_read) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: buffer-port read with no read "
                          "data phase armed\n", __func__);
            return 0;
        }
        sdbus_read_data(&s->sdbus, &w, 4);
        s->data_pos += 4;
        if (s->data_pos >= s->data_len) {
            s->data_len = 0;
            s->regs[USDHC_INT_STATUS >> 2] |= INT_TC;
            mcxn_usdhc_update_irq(s);
        }
        return le32_to_cpu(w);
    }
    case USDHC_HOST_CTRL_CAP:
        return HOST_CTRL_CAP_VALUE;
    case USDHC_DLL_STATUS:
        /*
         * A hardcoded `return 0` here silently beat the reset table.  RM reset
         * 0x200: the DLL's reference-select tap at its power-on position.  (The
         * LOCK bits are 0 in the RM too -- this is not a "never locks" bug.)
         */
        return s->regs[USDHC_DLL_STATUS / 4];
    case USDHC_ADMA_ERR_STATUS:
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
        /* Issue it to the real card on the bus. */
        s->regs[off >> 2] = val;
        mcxn_usdhc_send_command(s, val);
        return;
    case USDHC_DATA_BUFF_ACC_PORT: {
        /* PIO write: straight out to the card. */
        uint32_t w = cpu_to_le32(val);

        if (s->data_len == 0 || s->data_read) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: buffer-port write with no "
                          "write data phase armed\n", __func__);
            return;
        }
        sdbus_write_data(&s->sdbus, &w, 4);
        s->data_pos += 4;
        if (s->data_pos >= s->data_len) {
            s->data_len = 0;
            s->regs[USDHC_INT_STATUS >> 2] |= INT_TC;
            mcxn_usdhc_update_irq(s);
        }
        return;
    }
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

/*
 * USDHC0 reset values, from the RM's register map (generated by
 * tests/mcxn-reset-values/extract-rm-golden.py -- derived, never invented).
 */
static const struct { uint16_t off; uint32_t val; } rst_usdhc0[] = {
    { 0x004, 0x00010000u },   /* BLK_ATT */
    { 0x028, 0x08800020u },   /* PROT_CTRL */
    { 0x02C, 0x0080800Fu },   /* SYS_CTRL */
    { 0x040, 0x07F3B407u },   /* HOST_CTRL_CAP */
    { 0x044, 0x08100810u },   /* WTMK_LVL */
    { 0x048, 0x80000000u },   /* MIX_CTRL */
    { 0x064, 0x00000200u },   /* DLL_STATUS */
    { 0x0C0, 0x30007809u },   /* VEND_SPEC */
    { 0x0C8, 0x00019006u },   /* VEND_SPEC2 */
    { 0x0CC, 0x00212800u },   /* TUNING_CTRL */
};

static void mcxn_usdhc_reset(DeviceState *dev)
{
    MCXNUSDHCState *s = MCXN_USDHC(dev);
    int rst_i;

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x064 / 4] = 0x00000200u;   /* DLL_STATUS -- RM reset, derived */
    for (rst_i = 0; rst_i < (int)ARRAY_SIZE(rst_usdhc0); rst_i++) {
        s->regs[rst_usdhc0[rst_i].off / 4] = rst_usdhc0[rst_i].val;
    }
    s->regs[USDHC_HOST_CTRL_CAP >> 2] = HOST_CTRL_CAP_VALUE;
    s->data_len = 0;
    s->data_pos = 0;
    s->data_read = false;
}

static void mcxn_usdhc_realize(DeviceState *dev, Error **errp)
{
    MCXNUSDHCState *s = MCXN_USDHC(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_usdhc_ops, s,
                          TYPE_MCXN_USDHC, MCXN_USDHC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /*
     * Supply the BUS, as the silicon does.  We do NOT put a card on it: the
     * board or the operator attaches a real sd-card, and every response and
     * every byte then comes from that card rather than from this model.
     */
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, dev, "sd-bus");
}

static const VMStateDescription vmstate_mcxn_usdhc = {
    .name = TYPE_MCXN_USDHC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNUSDHCState, MCXN_USDHC_SIZE / 4),
        VMSTATE_UINT32(data_len, MCXNUSDHCState),
        VMSTATE_UINT32(data_pos, MCXNUSDHCState),
        VMSTATE_BOOL(data_read, MCXNUSDHCState),
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
