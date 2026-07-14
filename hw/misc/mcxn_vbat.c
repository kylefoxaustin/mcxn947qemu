/*
 * NXP MCX N VBAT (VBAT / always-on domain) — bring-up model.
 *
 * The VBAT block hosts the always-on (RTC backup) resources: the 16 kHz FRO and
 * the 32 kHz crystal oscillator, the RAM LDO and the tamper/CLKMON logic.
 * Firmware that enables the oscillator or LDO polls STATUSA for the matching
 * "ready" bit; the model reports those bits ready so init never spins. STATUSA
 * and STATUSB carry sticky flag bits that are write-1-to-clear. VERID returns a
 * constant. Remaining registers are backed permissively. Offsets/bits from the
 * MCXN947 CMSIS header (VBAT_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_vbat.h"
#include "migration/vmstate.h"

/* Register offsets */
#define VBAT_VERID    0x00   /* RO */
#define VBAT_STATUSA  0x10
#define VBAT_STATUSB  0x14

/*
 * STATUSA status/ready bits. LDO_RDY and OSC_RDY are level "ready" indicators
 * the driver polls after enabling the LDO/oscillator; the flag bits below are
 * sticky write-1-to-clear event flags.
 */
#define VBAT_STATUSA_POR_DET      (1u << 0)
#define VBAT_STATUSA_WAKEUP_FLAG  (1u << 1)
#define VBAT_STATUSA_TIMER0_FLAG  (1u << 2)
#define VBAT_STATUSA_TIMER1_FLAG  (1u << 3)
#define VBAT_STATUSA_LDO_RDY      (1u << 4)
#define VBAT_STATUSA_OSC_RDY      (1u << 5)

/* Sticky, write-1-to-clear flag bits in STATUSA. */
#define VBAT_STATUSA_W1C \
    (VBAT_STATUSA_POR_DET | VBAT_STATUSA_WAKEUP_FLAG | \
     VBAT_STATUSA_TIMER0_FLAG | VBAT_STATUSA_TIMER1_FLAG | \
     (1u << 6)  /* CLOCK_DET  */ | \
     (1u << 7)  /* CONFIG_DET */ | \
     (1u << 8)  /* VOLT_DET   */ | \
     (1u << 9)  /* TEMP_DET   */ | \
     (1u << 10) /* LIGHT_DET  */ | \
     (1u << 12) /* SEC0_DET   */ | \
     (1u << 16) /* IRQ0_DET   */ | \
     (1u << 17) /* IRQ1_DET   */ | \
     (1u << 18) /* IRQ2_DET   */ | \
     (1u << 19) /* IRQ3_DET   */)

/* Level "ready" bits that always read ready in the model. */
#define VBAT_STATUSA_READY (VBAT_STATUSA_LDO_RDY | VBAT_STATUSA_OSC_RDY)
#define VBAT_STATUSA_POR_DET (1u << 0)   /* a power-on reset really did just happen */

/*
 * The ENABLE bits the READY bits must follow.
 *
 * ⚠ `VBAT_LDOCTLA` USED TO BE 0x200.  THAT IS FROCTLA -- THE 16 kHz FRO's CONTROL
 *   REGISTER.  So STATUSA[LDO_RDY] -- the bit firmware polls to learn that the
 *   RETENTION LDO has come up -- was gated on bit 1 of a DIFFERENT PERIPHERAL.
 *
 *   Caught by the reset-value golden, which carries the RM's ADDRESSES (via CMSIS)
 *   and not the ones I typed: the manual puts OSCCTLA at 0x100, FROCTLA at 0x200 and
 *   LDOCTLA at 0x300, and the CMSIS VBAT_Type struct agrees to the byte.  TWO sources
 *   I did not author, against one number I did.
 *
 *   ⭐ AN ORACLE YOU DID NOT AUTHOR IS THE ONLY THING THAT CAN CATCH AN ADDRESS YOU
 *      INVENTED -- a test written against the model's own #define would have agreed
 *      with the bug forever.  (Same class as LPI2C at +0x000 where silicon says +0x800.)
 */
#define VBAT_OSCCTLA   0x100
#define VBAT_LDOCTLA   0x300
#define OSCCTLA_OSC_EN (1u << 0)
#define LDOCTLA_LDO_EN (1u << 1)

/*
 * RM reset: STATUSA = 0x81.  Bit 0 is POR_DET -- and that one is TRUE, a power-on
 * reset genuinely just happened.  LDO_RDY (bit 4) and OSC_RDY (bit 5) are NOT set.
 */
#define VBAT_STATUSA_RESET 0x00000081u

#define VBAT_VERID_VALUE  0x02000000u

/*
 * ⚠ VBAT IS WRITE-PROTECTED BY AN INVERSE-PAIR HANDSHAKE, AND WE WERE IGNORING IT.
 *
 * RM 36.x, verbatim:
 *
 *   "The VBAT registers are implemented as separate A and B registers.  When
 *    configuring an A register, you must write the inverse value to the
 *    corresponding B register."
 *
 * VBAT is the ALWAYS-ON domain -- the 16 kHz FRO, the 32 kHz crystal, the retention
 * LDO.  The A/B complement pair is a HARDWARE GUARD against a spurious write
 * corrupting the one power domain that survives reset.  The RM's own init sequence:
 *
 *      1. Write 7h to LDOCTLA.
 *      2. Write 0h to LDOCTLB[INVERSE].          <- WITHOUT THIS, NOTHING HAPPENS
 *      3. Wait for STATUSA[LDO_RDY] to become 1.
 *
 * ⭐ WE ACCEPTED THE BARE `LDOCTLA = 7` AND BROUGHT THE LDO UP.  SILICON DOES NOT.
 *    A model that is TOO FORGIVING does not fail safe -- IT SHIPS THE BUG TO THE
 *    HARDWARE.  Firmware that skipped step 2 worked perfectly here and would have
 *    died on the bench, and the developer would have trusted us over the board.
 *
 * THE GOLDEN PROVED THE MECHANISM RATHER THAN ME ASSUMING IT: all nine config pairs
 * in the RM's reset column satisfy  B == ~A  within the INVERSE mask, exactly.  The
 * one A register with a NONZERO reset (FROCTLA = 1, the FRO16K runs at power-on) is
 * the one B register that resets to ZERO.  Nine independent confirmations of a
 * mechanism I inferred from a CMSIS field name.  (STATUSB is NOT in the table: it is
 * a STATUS register, and the rule says "when CONFIGURING an A register".)
 */
#define VBAT_IRQENA   0x018
#define VBAT_IRQENB   0x01C
#define VBAT_WAKENA   0x020
#define VBAT_WAKENB   0x024
#define VBAT_OSCCTLB  0x104
#define VBAT_OSCCFGA  0x108
#define VBAT_OSCCFGB  0x10C
#define VBAT_FROCTLA  0x200
#define VBAT_OSCLCKA  0x118
#define VBAT_OSCLCKB  0x11C
#define VBAT_FROCTLB  0x204
#define VBAT_FROLCKA  0x218
#define VBAT_FROLCKB  0x21C
#define VBAT_LDOCTLB  0x304
#define VBAT_LDOLCKA  0x318
#define VBAT_LDOLCKB  0x31C

#define VBAT_LOCK_BIT 0x1u

/* (A, B, INVERSE mask) -- the mask widths are CMSIS VBAT_*_INVERSE_MASK, not guesses. */
static const struct { uint16_t a, b; uint32_t mask; } vbat_pair[] = {
    { VBAT_IRQENA,  VBAT_IRQENB,  0x000FFFFFu },
    { VBAT_WAKENA,  VBAT_WAKENB,  0x000FFFFFu },
    { VBAT_OSCCTLA, VBAT_OSCCTLB, 0x000FFFFFu },
    { VBAT_OSCCFGA, VBAT_OSCCFGB, 0x00000FFFu },
    { VBAT_FROCTLA, VBAT_FROCTLB, 0x00000001u },
    { VBAT_LDOCTLA, VBAT_LDOCTLB, 0x00000007u },
    { VBAT_OSCLCKA, VBAT_OSCLCKB, 0x00000001u },
    { VBAT_FROLCKA, VBAT_FROLCKB, 0x00000001u },
    { VBAT_LDOLCKA, VBAT_LDOLCKB, 0x00000001u },
};

/* RM reset values.  Derived from the manual, never invented. */
static const struct { uint16_t off; uint32_t val; } vbat_reset[] = {
    { 0x010, 0x00000081u },   /* STATUSA  (POR_DET: a reset really did just happen) */
    { 0x014, 0x000F003Eu },   /* STATUSB                                            */
    { 0x01C, 0x000FFFFFu },   /* IRQENB   = ~IRQENA                                 */
    { 0x024, 0x000FFFFFu },   /* WAKENB   = ~WAKENA                                 */
    { 0x104, 0x000FFFFFu },   /* OSCCTLB  = ~OSCCTLA                                */
    { 0x10C, 0x00000FFFu },   /* OSCCFGB  = ~OSCCFGA                                */
    { 0x11C, 0x00000001u },   /* OSCLCKB  = ~OSCLCKA                                */
    { 0x200, 0x00000001u },   /* FROCTLA   THE FRO16K IS RUNNING AT POWER-ON        */
    { 0x204, 0x00000000u },   /* FROCTLB  = ~FROCTLA                                */
    { 0x21C, 0x00000001u },   /* FROLCKB  = ~FROLCKA                                */
    { 0x304, 0x00000007u },   /* LDOCTLB  = ~LDOCTLA                                */
    { 0x31C, 0x00000001u },   /* LDOLCKB  = ~LDOLCKA                                */
};

/* The guest configured this pair correctly: B holds the inverse of A. */
static bool vbat_pair_ok(MCXNVBATState *s, uint16_t a, uint16_t b, uint32_t mask)
{
    return ((s->regs[a >> 2] ^ s->regs[b >> 2]) & mask) == mask;
}

/* Is the block owning register `off` LOCKED?  A lock is itself an A/B pair. */
static bool vbat_locked(MCXNVBATState *s, hwaddr off)
{
    uint16_t la, lb;

    if (off == VBAT_OSCCTLA || off == VBAT_OSCCTLB ||
        off == VBAT_OSCCFGA || off == VBAT_OSCCFGB) {
        la = VBAT_OSCLCKA; lb = VBAT_OSCLCKB;
    } else if (off == VBAT_FROCTLA || off == VBAT_FROCTLB) {
        la = VBAT_FROLCKA; lb = VBAT_FROLCKB;
    } else if (off == VBAT_LDOCTLA || off == VBAT_LDOCTLB) {
        la = VBAT_LDOLCKA; lb = VBAT_LDOLCKB;
    } else {
        return false;
    }

    /* Locked only when the LOCK pair itself is a VALID inverse pair with LOCK set. */
    return (s->regs[la >> 2] & VBAT_LOCK_BIT) &&
           !(s->regs[lb >> 2] & VBAT_LOCK_BIT);
}

static uint64_t mcxn_vbat_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNVBATState *s = MCXN_VBAT(opaque);
    uint32_t v = (off < MCXN_VBAT_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case VBAT_VERID:
        return VBAT_VERID_VALUE;
    case VBAT_STATUSA: {
        /*
         * ⚠ THIS USED TO BE:  return v | VBAT_STATUSA_READY;
         *                     -- "Oscillator and LDO are always ready in the model."
         *
         * "Always ready" is a FABRICATED ASSERTION, and it is the THIRD instance of
         * this exact class in this tree today -- SCG reported four oscillators VALID
         * that nobody turned on, and SYSCON handed out a 1 MHz clock nobody selected.
         * Every time, the mechanism was the same: WE GAVE THE GUEST SOMETHING IT HAD
         * NOT EARNED, and the guest believed us.
         *
         * The RM agrees this was wrong: STATUSA resets to 0x81, and LDO_RDY (bit 4)
         * and OSC_RDY (bit 5) are NOT among those bits.  Bit 0 (POR_DET) IS -- and
         * that one is TRUE: a power-on reset really did just happen.
         *
         * So READY now FOLLOWS ENABLE.  Firmware still never spins -- the LDO and the
         * oscillator are ready the instant they are enabled, which is the right
         * emulation of a settling time we do not model -- but they are NOT ready
         * BEFORE that, and code that asks "is the 32 kHz crystal running?" now gets
         * the truth instead of a yes it never asked for.
         */
        uint32_t rdy = 0;

        /*
         * READY follows ENABLE *AND THE INVERSE-PAIR HANDSHAKE*.  Enabling the LDO is
         * a TWO-REGISTER operation on this silicon (LDOCTLA = 7, then LDOCTLB = 0);
         * a guest that writes only the A half has not enabled anything, and the RM's
         * own init sequence polls LDO_RDY precisely to find that out.
         *
         * So firmware that skips the B write now spins here -- exactly as it would on
         * the board.  That is not us hanging the driver; that is the driver
         * discovering, in the emulator, the bug it would otherwise have shipped.
         * We ALSO say so on the operator's channel, because a spin with no explanation
         * is a bad way to learn it.
         */
        if ((s->regs[VBAT_OSCCTLA >> 2] & OSCCTLA_OSC_EN) &&
            vbat_pair_ok(s, VBAT_OSCCTLA, VBAT_OSCCTLB, 0x000FFFFFu)) {
            rdy |= VBAT_STATUSA_OSC_RDY;
        }
        if ((s->regs[VBAT_LDOCTLA >> 2] & LDOCTLA_LDO_EN) &&
            vbat_pair_ok(s, VBAT_LDOCTLA, VBAT_LDOCTLB, 0x00000007u)) {
            rdy |= VBAT_STATUSA_LDO_RDY;
        }

        if ((s->regs[VBAT_LDOCTLA >> 2] & LDOCTLA_LDO_EN) &&
            !vbat_pair_ok(s, VBAT_LDOCTLA, VBAT_LDOCTLB, 0x00000007u)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mcxn_vbat: LDOCTLA enabled but LDOCTLB is not its inverse "
                          "(A=0x%x B=0x%x) -- silicon requires BOTH writes; LDO_RDY "
                          "will not assert\n",
                          s->regs[VBAT_LDOCTLA >> 2], s->regs[VBAT_LDOCTLB >> 2]);
        }
        return (v & ~VBAT_STATUSA_READY) | rdy;
    }
    default:
        return v;
    }
}

static void mcxn_vbat_write(void *opaque, hwaddr off,
                            uint64_t value, unsigned size)
{
    MCXNVBATState *s = MCXN_VBAT(opaque);

    if (off >= MCXN_VBAT_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    /*
     * ⭐ ONCE A VBAT BLOCK IS LOCKED, SILICON REFUSES THE WRITE.  We used to take it.
     *   "More permissive than the hardware" is not the safe direction -- it means
     *   firmware that violates the lock works here and fails on the board, and the
     *   developer trusts us over the silicon.  Fault to the GUEST (the write does not
     *   land, so the register reads back unchanged and the guest can SEE it) and tell
     *   the operator why.
     */
    if (vbat_locked(s, off)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mcxn_vbat: write 0x%" PRIx64 " to LOCKED register @0x%" HWADDR_PRIx
                      " REFUSED (its LCKA[LOCK] pair is set)\n", value, off);
        return;
    }

    switch (off) {
    case VBAT_VERID:
        return;  /* read-only */
    case VBAT_STATUSA:
        /* Write-1-to-clear the sticky flag bits; ready bits are not stored. */
        s->regs[off >> 2] &= ~((uint32_t)value & VBAT_STATUSA_W1C);
        return;
    case VBAT_STATUSB:
        /* STATUSB mirrors STATUSA flags (inverse); treat as write-1-to-clear. */
        s->regs[off >> 2] &= ~(uint32_t)value;
        return;
    default:
        s->regs[off >> 2] = value;
        return;
    }
}

static const MemoryRegionOps mcxn_vbat_ops = {
    .read = mcxn_vbat_read,
    .write = mcxn_vbat_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_vbat_reset(DeviceState *dev)
{
    MCXNVBATState *s = MCXN_VBAT(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < ARRAY_SIZE(vbat_reset); i++) {
        s->regs[vbat_reset[i].off / 4] = vbat_reset[i].val;
    }

    /*
     * THE INVARIANT IS THE POINT, SO ASSERT IT RATHER THAN TRUSTING MY OWN TYPING.
     *
     * Every VBAT config pair must come out of reset with B == ~A within its INVERSE
     * mask -- that is what makes the reset state a VALID configuration rather than a
     * corrupt one.  All nine pairs in the RM's reset column satisfy it exactly; if a
     * future edit to vbat_reset[] breaks one, this trips HERE, at reset, instead of
     * silently handing the guest a pair the silicon would reject.
     *
     * (A reset table is a claim.  This is the claim checking itself.)
     */
    for (i = 0; i < ARRAY_SIZE(vbat_pair); i++) {
        assert(vbat_pair_ok(s, vbat_pair[i].a, vbat_pair[i].b, vbat_pair[i].mask));
    }
    /* RM: STATUSA resets to 0x81 -- POR_DET set (true!), the READY bits clear. */
    s->regs[VBAT_STATUSA >> 2] = VBAT_STATUSA_RESET;
}

static void mcxn_vbat_realize(DeviceState *dev, Error **errp)
{
    MCXNVBATState *s = MCXN_VBAT(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_vbat_ops, s,
                          TYPE_MCXN_VBAT, MCXN_VBAT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_vbat = {
    .name = TYPE_MCXN_VBAT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNVBATState, MCXN_VBAT_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_vbat_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_vbat_realize;
    device_class_set_legacy_reset(dc, mcxn_vbat_reset);
    dc->vmsd = &vmstate_mcxn_vbat;
}

static const TypeInfo mcxn_vbat_types[] = {
    {
        .name          = TYPE_MCXN_VBAT,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNVBATState),
        .class_init    = mcxn_vbat_class_init,
    },
};

DEFINE_TYPES(mcxn_vbat_types)
