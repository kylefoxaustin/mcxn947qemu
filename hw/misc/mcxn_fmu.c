/*
 * NXP MCX N FMU (Flash Management Unit) — functional flash controller.  See
 * header for the program/erase protocol and why it is modelled this way.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_fmu.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define R_FSTAT   0x0
#define R_FCNFG   0x4
#define R_FCTRL   0x8
#define R_FCCOB0  0x10   /* FCCOB0..7 @ 0x10..0x2C */

/* FSTAT, CMSIS-exact (FMU_FSTAT_*). */
#define FSTAT_FAIL   (1u << 0)
#define FSTAT_CMDABT (1u << 2)
#define FSTAT_PVIOL  (1u << 4)
#define FSTAT_ACCERR (1u << 5)
#define FSTAT_CCIF   (1u << 7)   /* 1 = command complete / idle           */
#define FSTAT_PERDY  (1u << 31)  /* 1 = staged op ready, controller stalled */
#define FSTAT_PEWEN_SHIFT 24
#define FSTAT_PEWEN_MASK  (3u << FSTAT_PEWEN_SHIFT)
#define PEWEN_OFF    0u          /* writes to flash space not enabled     */
#define PEWEN_PHRASE 1u          /* one phrase (phrase program, sector erase) */
#define PEWEN_PAGE   2u          /* one page (page program)               */

/* The error flags that block a new command from launching (§8.3.1.1.2). */
#define FSTAT_ERRORS (FSTAT_FAIL | FSTAT_CMDABT | FSTAT_PVIOL | FSTAT_ACCERR)

#define FCNFG_CCIE   (1u << 7)
#define FCTRL_ABTREQ (1u << 24)

/* FCMD command codes (RM Table 38, command summary). */
#define FCMD_RD1ALL  0x00
#define FCMD_RD1BLK  0x01
#define FCMD_RD1SCR  0x02
#define FCMD_RD1PG   0x03
#define FCMD_RD1PHR  0x04
#define FCMD_PGMPG   0x23
#define FCMD_PGMPHR  0x24
#define FCMD_ERSALL  0x40
#define FCMD_ERSSCR  0x42

static uint8_t *fmu_flash_ptr(MCXNFMUState *s)
{
    return s->flash ? memory_region_get_ram_ptr(s->flash) : NULL;
}

/*
 * Map an FCCOB-supplied flash address to an offset into the array.  The RM says
 * FCCOB addresses assume flash starts at 0, but firmware routinely passes an
 * address from the secure (0x1000_0000) aperture; masking by the (power-of-two)
 * size accepts either without ambiguity.
 */
static uint64_t fmu_flash_off(MCXNFMUState *s, uint64_t addr)
{
    return s->flash_size ? (addr & (s->flash_size - 1)) : 0;
}

static void fmu_update_irq(MCXNFMUState *s)
{
    /* The interrupt asserts when the command completes and CCIE is set. */
    qemu_set_irq(s->irq,
                 !!(s->fstat & FSTAT_CCIF) && !!(s->fcnfg & FCNFG_CCIE));
}

/* Finish the current command: close any write window and re-assert CCIF. */
static void fmu_complete(MCXNFMUState *s)
{
    s->pe_active = false;
    s->pe_words  = 0;
    s->fstat &= ~(FSTAT_PEWEN_MASK | FSTAT_PERDY);
    s->fstat |= FSTAT_CCIF;
    fmu_update_irq(s);
}

/* Abort the in-flight command with an error flag already set. */
static void fmu_fail(MCXNFMUState *s, uint32_t flag)
{
    s->fstat |= flag;
    fmu_complete(s);
}

/* Open the PEWEN write window for a program or sector-erase command. */
static void fmu_open_write_window(MCXNFMUState *s, uint8_t cmd,
                                  uint32_t words, uint32_t pewen)
{
    s->pe_cmd    = cmd;
    s->pe_expect = words;
    s->pe_words  = 0;
    s->pe_addr   = 0;
    s->pe_active = true;
    memset(s->pe_buf, 0xFF, sizeof(s->pe_buf));

    /* CCIF stays clear (busy) for the whole handshake. */
    s->fstat &= ~FSTAT_PERDY;
    s->fstat = (s->fstat & ~FSTAT_PEWEN_MASK) | (pewen << FSTAT_PEWEN_SHIFT);
}

/* Verify a span of flash reads as erased; set FAIL if not (Read 1s family). */
static void fmu_verify_erased(MCXNFMUState *s, uint64_t off, uint64_t len)
{
    uint8_t *flash = fmu_flash_ptr(s);

    if (!flash) {
        return;
    }
    for (uint64_t i = 0; i < len && off + i < s->flash_size; i++) {
        if (flash[off + i] != 0xFF) {
            s->fstat |= FSTAT_FAIL;
            return;
        }
    }
}

/*
 * Commit the staged program/erase once the guest clears PERDY.  This is where
 * real flash physics live: programming only clears bits, and the readback
 * verify is what makes cumulative programming (no intervening erase) fail.
 */
static void fmu_commit(MCXNFMUState *s)
{
    uint8_t *flash = fmu_flash_ptr(s);
    uint64_t len = s->pe_expect * 4;
    uint64_t off = s->pe_addr;

    if (!flash) {
        fmu_complete(s);
        return;
    }

    switch (s->pe_cmd) {
    case FCMD_PGMPG:
    case FCMD_PGMPHR:
        if (off + len > s->flash_size) {
            fmu_fail(s, FSTAT_ACCERR);
            return;
        }
        for (uint64_t i = 0; i < len; i++) {
            /* Flash programs 1 -> 0 only; it cannot restore a cleared bit. */
            uint8_t merged = flash[off + i] & s->pe_buf[i];

            flash[off + i] = merged;
            if (merged != s->pe_buf[i]) {
                /*
                 * A bit we were asked to leave at 1 read back 0: verify fails.
                 * This is precisely the back-to-back-program-without-erase case
                 * the RM forbids (§8.3.2.11 CAUTION).
                 */
                s->fstat |= FSTAT_FAIL;
            }
        }
        memory_region_flush_rom_device(s->flash, off, len);
        break;

    case FCMD_ERSSCR: {
        /*
         * The sector is selected by the phrase the guest wrote, not by
         * FCCOB.
         */
        uint64_t sec = off & ~(uint64_t)(MCXN_FLASH_SECTOR - 1);

        if (sec + MCXN_FLASH_SECTOR > s->flash_size) {
            fmu_fail(s, FSTAT_ACCERR);
            return;
        }
        memset(flash + sec, 0xFF, MCXN_FLASH_SECTOR);
        memory_region_flush_rom_device(s->flash, sec, MCXN_FLASH_SECTOR);
        fmu_verify_erased(s, sec, MCXN_FLASH_SECTOR);
        break;
    }

    default:
        g_assert_not_reached();
    }

    fmu_complete(s);
}

/* Execute the command staged in FCCOB (launched by clearing CCIF). */
static void fmu_launch(MCXNFMUState *s)
{
    uint8_t cmd = s->fccob[0] & 0xFF;
    /*
     * The address parameter, where a command takes one, is FCCOB2. FCCOB1 is
     * command options (§8.3.1.1.1) — reading the address from FCCOB1 would
     * silently target the wrong sector.
     */
    uint64_t addr = fmu_flash_off(s, s->fccob[2]);
    uint8_t *flash = fmu_flash_ptr(s);

    /* CCIF is clear for the duration of the command. */
    s->fstat &= ~FSTAT_CCIF;
    fmu_update_irq(s);

    switch (cmd) {
    case FCMD_ERSALL:
        if (flash) {
            memset(flash, 0xFF, s->flash_size);
            memory_region_flush_rom_device(s->flash, 0, s->flash_size);
        }
        fmu_complete(s);
        return;

    case FCMD_RD1ALL:
        fmu_verify_erased(s, 0, s->flash_size);
        fmu_complete(s);
        return;

    case FCMD_RD1BLK:
        /* The MCX N947's 2 MB main array is two 1 MB blocks. */
        fmu_verify_erased(s, addr & ~(uint64_t)(s->flash_size / 2 - 1),
                          s->flash_size / 2);
        fmu_complete(s);
        return;

    case FCMD_RD1SCR:
        if (addr & (MCXN_FLASH_SECTOR - 1)) {
            fmu_fail(s, FSTAT_ACCERR);   /* address not sector aligned */
            return;
        }
        fmu_verify_erased(s, addr, MCXN_FLASH_SECTOR);
        fmu_complete(s);
        return;

    case FCMD_RD1PG:
        if (addr & (MCXN_FLASH_PAGE - 1)) {
            fmu_fail(s, FSTAT_ACCERR);   /* address not page aligned */
            return;
        }
        fmu_verify_erased(s, addr, MCXN_FLASH_PAGE);
        fmu_complete(s);
        return;

    case FCMD_RD1PHR:
        if (addr & (MCXN_FLASH_PHRASE - 1)) {
            fmu_fail(s, FSTAT_ACCERR);   /* address not phrase aligned */
            return;
        }
        fmu_verify_erased(s, addr, MCXN_FLASH_PHRASE);
        fmu_complete(s);
        return;

    /*
     * These three do not complete here: they open a write window and stall
     * until the guest has supplied the address+data as stores to flash.
     */
    case FCMD_PGMPG:
        fmu_open_write_window(s, cmd, MCXN_FLASH_PAGE / 4, PEWEN_PAGE);
        return;

    case FCMD_PGMPHR:
    case FCMD_ERSSCR:
        fmu_open_write_window(s, cmd, MCXN_FLASH_PHRASE / 4, PEWEN_PHRASE);
        return;

    default:
        fmu_fail(s, FSTAT_ACCERR);       /* unrecognized command code */
        return;
    }
}

/*
 * A guest store into the flash address space.  On silicon these only take
 * effect inside an open PEWEN window; anywhere else they are trapped outside
 * the flash module and do nothing.
 */
static void mcxn_fmu_flash_write(void *opaque, hwaddr off, uint64_t val,
                                 unsigned size)
{
    MCXNFMUState *s = MCXN_FMU(opaque);
    uint32_t align;

    if (!s->pe_active || !(s->fstat & FSTAT_PEWEN_MASK)) {
        /*
         * Not a bug in QEMU — a bug in the guest.  Real flash would ignore
         * this, so we ignore it too, loudly.
         */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mcxn-fmu: store to flash 0x%" HWADDR_PRIx " outside a "
                      "program/erase window is ignored (flash is not RAM; use "
                      "the FMU PGMPHR/PGMPG command sequence)\n", off);
        return;
    }

    if (size != 4) {
        /* The RM specifies the write phase in words. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mcxn-fmu: %u-byte store to flash during a program/erase "
                      "window; only 32-bit writes are allowed\n", size);
        fmu_fail(s, FSTAT_ACCERR);
        return;
    }

    if (s->pe_words == 0) {
        /* First store fixes the address, and must be aligned to the unit. */
        align = s->pe_expect * 4;
        if (off & (align - 1)) {
            fmu_fail(s, FSTAT_ACCERR);
            return;
        }
        s->pe_addr = off;
    } else if (off != s->pe_addr + s->pe_words * 4) {
        /* The RM requires N *consecutive* words. */
        fmu_fail(s, FSTAT_ACCERR);
        return;
    }

    if (s->pe_words >= s->pe_expect) {
        fmu_fail(s, FSTAT_ACCERR);   /* more writes than expected */
        return;
    }

    stl_le_p(&s->pe_buf[s->pe_words * 4], (uint32_t)val);
    s->pe_words++;

    if (s->pe_words == s->pe_expect) {
        /*
         * Window closes; stall until the guest clears PERDY.  Note the staged
         * data is deliberately NOT in the array yet — the RM says it is not
         * readable until the operation completes.
         */
        s->fstat &= ~FSTAT_PEWEN_MASK;
        s->fstat |= FSTAT_PERDY;
    }
}

static uint64_t mcxn_fmu_flash_read(void *opaque, hwaddr off, unsigned size)
{
    /*
     * Unreachable in romd mode: reads and fetches go straight to the backing
     * RAM.  Present only because a ROM device must supply read ops.
     */
    return 0;
}

static const MemoryRegionOps mcxn_fmu_flash_ops = {
    .read = mcxn_fmu_flash_read,
    .write = mcxn_fmu_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void mcxn_fmu_init_flash(MCXNFMUState *s, Object *owner, MemoryRegion *flash,
                         uint64_t size, Error **errp)
{
    s->flash = flash;
    s->flash_size = size;

    /*
     * A ROM device, not RAM: reads and instruction fetch are direct (XIP and
     * the -kernel ROM loader, which uses address_space_write_rom(), bypass the
     * write op), while guest stores are routed to mcxn_fmu_flash_write().
     */
    memory_region_init_rom_device(flash, owner, &mcxn_fmu_flash_ops, s,
                                  "mcxn.flash", size, errp);
}

static uint64_t mcxn_fmu_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNFMUState *s = MCXN_FMU(opaque);

    switch (off) {
    case R_FSTAT: return s->fstat;
    case R_FCNFG: return s->fcnfg;
    case R_FCTRL: return s->fctrl;
    default:
        if (off >= R_FCCOB0 && off < R_FCCOB0 + 8 * 4) {
            return s->fccob[(off - R_FCCOB0) / 4];
        }
        return 0;
    }
}

static void mcxn_fmu_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    MCXNFMUState *s = MCXN_FMU(opaque);
    uint32_t v = val;
    bool blocked;

    switch (off) {
    case R_FSTAT:
        /*
         * FAIL/CMDABT/PVIOL/ACCERR are write-1-to-clear, and while any of them
         * is set a new command cannot launch (§8.3.1.1.2) — so clearing an
         * error and launching take two writes, as on silicon.  Decide on the
         * pre-write state.
         */
        blocked = s->fstat & FSTAT_ERRORS;
        s->fstat &= ~(v & FSTAT_ERRORS);

        /* PERDY is W1C, and clearing it is what commits a staged op. */
        if ((v & FSTAT_PERDY) && (s->fstat & FSTAT_PERDY)) {
            s->fstat &= ~FSTAT_PERDY;
            fmu_commit(s);
            return;
        }

        /* Writing 1 to CCIF clears it and launches the staged command. */
        if ((v & FSTAT_CCIF) && (s->fstat & FSTAT_CCIF) && !blocked) {
            fmu_launch(s);
        }
        return;

    case R_FCNFG:
        s->fcnfg = v;
        fmu_update_irq(s);
        return;

    case R_FCTRL:
        s->fctrl = v;
        if (v & FCTRL_ABTREQ) {
            /* Abort an in-flight program/erase (§8.3.2.11). */
            if (s->pe_active) {
                fmu_fail(s, FSTAT_CMDABT);
            }
            s->fctrl &= ~FCTRL_ABTREQ;   /* cleared automatically */
        }
        return;

    default:
        if (off >= R_FCCOB0 && off < R_FCCOB0 + 8 * 4) {
            /* While a command is running, writes to FCCOB are ignored. */
            if (s->fstat & FSTAT_CCIF) {
                s->fccob[(off - R_FCCOB0) / 4] = v;
            }
        }
        return;
    }
}

static const MemoryRegionOps mcxn_fmu_ops = {
    .read = mcxn_fmu_read,
    .write = mcxn_fmu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_fmu_reset(DeviceState *dev)
{
    MCXNFMUState *s = MCXN_FMU(dev);

    s->fstat = FSTAT_CCIF;          /* idle, command complete */
    s->fcnfg = 0;
    s->fctrl = 0;
    memset(s->fccob, 0, sizeof(s->fccob));

    s->pe_active = false;
    s->pe_words  = 0;
    s->pe_expect = 0;
    s->pe_addr   = 0;
    s->pe_cmd    = 0;
    memset(s->pe_buf, 0xFF, sizeof(s->pe_buf));
}

static void mcxn_fmu_realize(DeviceState *dev, Error **errp)
{
    MCXNFMUState *s = MCXN_FMU(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_fmu_ops, s,
                          TYPE_MCXN_FMU, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

/* Re-drive the IRQ line from restored register state after migration: the
 * output line is not part of vmstate, so a VM migrated with an asserted IRQ
 * would otherwise land with the line low and the guest's level IRQ lost. */
static int mcxn_fmu_post_load(void *opaque, int version_id)
{
    fmu_update_irq(MCXN_FMU(opaque));
    return 0;
}

static const VMStateDescription vmstate_mcxn_fmu = {
    .name = TYPE_MCXN_FMU,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = mcxn_fmu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(fstat, MCXNFMUState),
        VMSTATE_UINT32(fcnfg, MCXNFMUState),
        VMSTATE_UINT32(fctrl, MCXNFMUState),
        VMSTATE_UINT32_ARRAY(fccob, MCXNFMUState, 8),
        VMSTATE_UINT8_ARRAY(pe_buf, MCXNFMUState, MCXN_FLASH_PAGE),
        VMSTATE_UINT64(pe_addr, MCXNFMUState),
        VMSTATE_UINT32(pe_words, MCXNFMUState),
        VMSTATE_UINT32(pe_expect, MCXNFMUState),
        VMSTATE_UINT8(pe_cmd, MCXNFMUState),
        VMSTATE_BOOL(pe_active, MCXNFMUState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_fmu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_fmu_realize;
    device_class_set_legacy_reset(dc, mcxn_fmu_reset);
    dc->vmsd = &vmstate_mcxn_fmu;
}

static const TypeInfo mcxn_fmu_types[] = {
    {
        .name          = TYPE_MCXN_FMU,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNFMUState),
        .class_init    = mcxn_fmu_class_init,
    },
};

DEFINE_TYPES(mcxn_fmu_types)
