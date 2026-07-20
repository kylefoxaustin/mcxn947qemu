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
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
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
#define SCG_READY    0x01000000u   /* xxxVLD / xxx_LOCK, bit 24 in every CSR */

/* Enable bits, from CMSIS -- the term the VALID bit must FOLLOW. */
#define SOSCCSR_SOSCEN    (1u << 0)
#define ROSCCSR_ROSCCM    (1u << 16)  /* ROSC has NO enable bit; see below */
#define APLLCSR_PWR_CLK   ((1u << 0) | (1u << 1))  /* APLLPWREN | APLLCLKEN */
#define SPLLCSR_PWR_CLK   ((1u << 0) | (1u << 1))  /* SPLLPWREN | SPLLCLKEN */

/* Additional register offsets (CMSIS SCG_Type). */
#define SCG_APLLNDIV      0x50C
#define SCG_APLLMDIV      0x510
#define SCG_APLLPDIV      0x514
#define SCG_APLLLOCK_CNFG 0x518
#define SCG_APLLSSCG1     0x528
#define SCG_SPLLNDIV      0x60C
#define SCG_SPLLMDIV      0x610
#define SCG_SPLLPDIV      0x614
#define SCG_SPLLLOCK_CNFG 0x618
#define SCG_SPLLSSCG1     0x628
#define SCG_LDOCSR        0x800
#define SCG_FIRCCFG       0x308

#define SIRCCSR_PERIPH_EN (1u << 5)   /* SIRC_CLK_PERIPH_EN */
#define FIRCCSR_FIRCEN    (1u << 0)
#define FIRCCFG_RANGE     (1u << 0)

#define FRO12M_HZ    12000000u
#define FROHF_48_HZ  48000000u
#define FROHF_144_HZ 144000000u
#define CLK48M_HZ    48000000u        /* FIRC fixed 48 MHz output (PLL source 1) */
#define EXTCLK_HZ    16000000u        /* SDK default s_Ext_Clk_Freq (SOSC source 0) */
#define OSC32K_HZ    32768u           /* ROSC / 32 kHz (PLL source 2) */

/* APLL/SPLL control + divider registers (CMSIS SCG_Type). */
#define SCG_APLLCTRL      0x504
#define SCG_SPLLCTRL      0x604
/* APLLCTRL / SPLLCTRL fields (identical layout). */
#define PLLCTRL_SOURCE_MASK    0x6000000u   /* [26:25]: 0 ExtClk, 1 Clk48M, 2 Osc32K */
#define PLLCTRL_SOURCE_SHIFT   25
#define PLLCTRL_BYPASSPOSTDIV2 0x10000u
#define PLLCTRL_BYPASSPREDIV   0x80000u
#define PLLCTRL_BYPASSPOSTDIV  0x100000u
#define PLLNDIV_MASK           0xFFu
#define PLLMDIV_MASK           0xFFFFu
#define PLLPDIV_MASK           0x1Fu
#define PLLSSCG1_SEL_SS_MDIV   0x800u

/* RCCR/CSR[SCS] main-clock source select (bits [27:24]). */
#define RCCR_SCS_MASK   0x0F000000u
#define RCCR_SCS_SHIFT  24
#define SCS_SOSC   1u   /* ExtClk / clk_in */
#define SCS_SIRC   2u   /* FRO_12M */
#define SCS_FIRC   3u   /* FRO_HF  */
#define SCS_ROSC   4u   /* Osc32K  */
#define SCS_APLL   5u   /* PLL0    */
#define SCS_SPLL   6u   /* PLL1    */
#define SCS_UPLL   7u   /* USB PLL (not modelled) */

/*
 * Publish the source clocks SCG actually produces.  This mirrors the SDK's own
 * logic exactly (fsl_clock.c):
 *
 *   CLOCK_GetFro12MFreq(): SIRCCSR[SIRC_CLK_PERIPH_EN] ? 12000000 : 0
 *   CLOCK_GetFroHfFreq() : !FIRCCSR[FIRCEN] ? 0
 *                        : FIRCCFG[RANGE]   ? 144000000 : 48000000
 *
 * ⚠ IT MUST MIRROR IT EXACTLY, AND THAT IS THE WHOLE POINT.  The GUEST computes the
 * rate from these same registers and programs its timers from the answer.  If our
 * timer ticks at a DIFFERENT rate than the one the driver computed, every delay it
 * derives is wrong BY THAT RATIO -- and nothing anywhere says a word.  That is not a
 * missing feature, it is a SILENT WRONG ANSWER, and it was live: CTIMER ignored its
 * selector and ran at sysclk, so a driver told "you are on FRO_HF, 48 MHz" got a
 * timer running at 150 MHz -- every CTIMER delay 3.1x too short.
 */
/* FRO 48 MHz output (PLL source 1 / Clk48M): available while the FIRC is enabled. */
static uint32_t scg_clk48m_hz(MCXNSCGState *s)
{
    return (s->regs[SCG_FIRCCSR >> 2] & FIRCCSR_FIRCEN) ? CLK48M_HZ : 0;
}

/*
 * One PLL's output frequency, from its own CTRL/NDIV/MDIV/PDIV registers.  Mirrors
 * the SDK exactly (fsl_clock.c CLOCK_GetPll0OutFreq + findPll0PreDiv/PostDiv/MMult):
 *
 *   Fout = (Fin / N) * M / postdiv
 *     N       = NDIV (min 1), or 1 if BYPASSPREDIV
 *     M       = MDIV (integer; the SSCG fractional path is only used with SEL_SS_MDIV)
 *     postdiv = 2*PDIV (min 2), or PDIV if BYPASSPOSTDIV2, or 1 if BYPASSPOSTDIV
 *   Fin per CTRL[SOURCE]: 0 ExtClk(SOSC), 1 Clk48M(FIRC 48M), 2 Osc32K(ROSC).
 *
 * Gated by the PLL being powered/locked (CSR PWREN|CLKEN) -- the same term the read
 * path uses to report the lock bit.  The APLL and SPLL share this layout, so one
 * helper serves both (ctrl/ndiv/mdiv/pdiv/sscg1/csr offsets passed in).
 */
static uint32_t scg_pll_out(MCXNSCGState *s, hwaddr ctrl, hwaddr ndiv, hwaddr mdiv,
                            hwaddr pdiv, hwaddr sscg1, hwaddr csr)
{
    uint32_t c = s->regs[ctrl >> 2];
    uint32_t fin, n, m, postdiv;

    if (!(s->regs[csr >> 2] & APLLCSR_PWR_CLK)) {
        return 0;                       /* not powered/enabled: no output */
    }
    switch ((c & PLLCTRL_SOURCE_MASK) >> PLLCTRL_SOURCE_SHIFT) {
    case 0:
        fin = (s->regs[SCG_SOSCCSR >> 2] & SOSCCSR_SOSCEN) ? EXTCLK_HZ : 0;
        break;
    case 1:
        fin = scg_clk48m_hz(s);
        break;
    case 2:
        fin = (s->regs[SCG_ROSCCSR >> 2] & ROSCCSR_ROSCCM) ? OSC32K_HZ : 0;
        break;
    default:
        fin = 0;
        break;
    }
    if (fin == 0) {
        return 0;
    }

    n = (c & PLLCTRL_BYPASSPREDIV) ? 1 : (s->regs[ndiv >> 2] & PLLNDIV_MASK);
    if (n == 0) {
        n = 1;
    }
    if (c & PLLCTRL_BYPASSPOSTDIV) {
        postdiv = 1;
    } else if (c & PLLCTRL_BYPASSPOSTDIV2) {
        postdiv = s->regs[pdiv >> 2] & PLLPDIV_MASK;
    } else {
        postdiv = 2 * (s->regs[pdiv >> 2] & PLLPDIV_MASK);
    }
    if (postdiv == 0) {
        postdiv = 2;
    }
    /* Integer M only.  SSCG fractional multiply (SEL_SS_MDIV) is not modelled; a
     * config that selects it would need the fractional path -- flag rather than
     * silently truncate. */
    if (s->regs[sscg1 >> 2] & PLLSSCG1_SEL_SS_MDIV) {
        qemu_log_mask(LOG_UNIMP, "mcxn-scg: PLL SSCG fractional multiply not "
                      "modelled; using integer MDIV\n");
    }
    m = s->regs[mdiv >> 2] & PLLMDIV_MASK;

    return (uint32_t)((uint64_t)(fin / n) * m / postdiv);
}

/*
 * The SCG main clock: RCCR[SCS] selects which source feeds the core/bus tree (the
 * M33 cpuclk and, through SYSCON's AHBCLKDIV, the bus clock).  Out of reset RCCR[SCS]
 * = 3 (FRO_HF), so an un-configured core runs at 48 MHz -- exactly as silicon does;
 * BOARD_InitBootClocks brings up PLL0 and switches SCS to 5, raising it to 150 MHz.
 */
static uint32_t scg_main_clk_hz(MCXNSCGState *s)
{
    uint32_t scs = (s->regs[SCG_RCCR >> 2] & RCCR_SCS_MASK) >> RCCR_SCS_SHIFT;

    switch (scs) {
    case SCS_SOSC:
        return (s->regs[SCG_SOSCCSR >> 2] & SOSCCSR_SOSCEN) ? EXTCLK_HZ : 0;
    case SCS_SIRC:
        return clock_get_hz(s->fro12m);
    case SCS_FIRC:
        return clock_get_hz(s->frohf);
    case SCS_ROSC:
        return (s->regs[SCG_ROSCCSR >> 2] & ROSCCSR_ROSCCM) ? OSC32K_HZ : 0;
    case SCS_APLL:
        return scg_pll_out(s, SCG_APLLCTRL, SCG_APLLNDIV, SCG_APLLMDIV,
                           SCG_APLLPDIV, SCG_APLLSSCG1, SCG_APLLCSR);
    case SCS_SPLL:
        return scg_pll_out(s, SCG_SPLLCTRL, SCG_SPLLNDIV, SCG_SPLLMDIV,
                           SCG_SPLLPDIV, SCG_SPLLSSCG1, SCG_SPLLCSR);
    default:
        qemu_log_mask(LOG_UNIMP,
                      "mcxn-scg: main clock source SCS=%u not modelled\n", scs);
        return 0;
    }
}

static void mcxn_scg_update_clocks(MCXNSCGState *s)
{
    uint32_t sirccsr = s->regs[SCG_SIRCCSR >> 2];
    uint32_t firccsr = s->regs[SCG_FIRCCSR >> 2];
    uint32_t firccfg = s->regs[SCG_FIRCCFG >> 2];
    uint32_t hf;

    clock_update_hz(s->fro12m,
                    (sirccsr & SIRCCSR_PERIPH_EN) ? FRO12M_HZ : 0);

    if (!(firccsr & FIRCCSR_FIRCEN)) {
        hf = 0;
    } else {
        hf = (firccfg & FIRCCFG_RANGE) ? FROHF_144_HZ : FROHF_48_HZ;
    }
    clock_update_hz(s->frohf, hf);

    /* PLL0/PLL1 outputs — derived from their own dividers (source per CTRL[SOURCE],
     * which for the 150 MHz setup is the 48 MHz FIRC just updated above). */
    clock_update_hz(s->apll, scg_pll_out(s, SCG_APLLCTRL, SCG_APLLNDIV, SCG_APLLMDIV,
                                         SCG_APLLPDIV, SCG_APLLSSCG1, SCG_APLLCSR));
    clock_update_hz(s->spll, scg_pll_out(s, SCG_SPLLCTRL, SCG_SPLLNDIV, SCG_SPLLMDIV,
                                         SCG_SPLLPDIV, SCG_SPLLSSCG1, SCG_SPLLCSR));

    /* Main clock last: it may select a PLL just updated above. */
    clock_update_hz(s->mainclk, scg_main_clk_hz(s));
}

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
    /*
     * ⚠ THESE USED TO BE ONE CASE:  return v | SCG_READY;   -- "always valid".
     *
     * "Always valid" is not a conservative simplification.  It is a FABRICATED
     * ASSERTION, and the SDK acts on it:
     *
     *     uint32_t CLOCK_GetExtClkFreq(void) {              -- fsl_clock.c:2321
     *         return ((SCG0->SOSCCSR & SCG_SOSCCSR_SOSCVLD_MASK) != 0UL)
     *                ? s_Ext_Clk_Freq : 0U;
     *     }
     *
     * -- so a VLD the guest never earned makes the SDK report an EXTERNAL CRYSTAL
     * FREQUENCY FOR AN OSCILLATOR NOBODY TURNED ON.  The RM agrees it is wrong:
     * SOSCCSR/ROSCCSR/APLLCSR/SPLLCSR all reset to 0.  (SIRC and FIRC are genuinely
     * running out of reset and carry their VLD in their RESET VALUE, which is where
     * it belongs -- see mcxn_scg_reset.)
     *
     * So VALID now FOLLOWS ENABLE.  Firmware still never spins -- the clock is
     * "ready" the instant it is enabled, which is the right emulation of a lock
     * time we do not model -- but it is not ready BEFORE THAT, and code that asks
     * "is this oscillator running?" now gets the truth.
     *
     * The enable term per source is taken from what THE DRIVER ACTUALLY SETS before
     * it spins, not from what the field is named:
     *
     *   SOSC:  sets SOSCCM|SOSCEN, waits SOSCVLD            (fsl_clock.c:236)
     *   ROSC:  HAS NO ENABLE BIT AT ALL.  The driver sets ROSCCM and waits ROSCVLD
     *          (fsl_clock.c:355).  A header is a claim about the silicon; a driver
     *          is a claim about what the silicon must DO.  The driver wins.
     *   APLL:  sets APLLPWREN|APLLCLKEN                     (fsl_clock.c:2163)
     *   SPLL:  sets SPLLPWREN|SPLLCLKEN                     (fsl_clock.c:2221)
     */
    case SCG_SOSCCSR:
        return (v & SOSCCSR_SOSCEN) ? (v | SCG_READY) : v;
    case SCG_ROSCCSR:
        return (v & ROSCCSR_ROSCCM) ? (v | SCG_READY) : v;
    case SCG_APLLCSR:
        return (v & APLLCSR_PWR_CLK) ? (v | SCG_READY) : v;
    case SCG_SPLLCSR:
        return (v & SPLLCSR_PWR_CLK) ? (v | SCG_READY) : v;

    /*
     * SIRC really is running out of reset -- its VLD (bit 24) is IN ITS RESET VALUE
     * (0x0100_0020), which is where it belongs.
     *
     * ⚠ FIRC IS NOT, AND I FABRICATED ITS VLD FOR A DAY BECAUSE I COULD NOT READ THE
     * RESET VALUE.  The RM's summary row for FIRCCSR says "See section", and the
     * section's bit diagram does not survive pdftotext -- so I left `v | SCG_READY`
     * with a note that I had no authoritative value and would not guess one.
     *
     * That was the right call about the VALUE and the wrong call about the BEHAVIOUR,
     * and the tool told me so only once it started COUNTING the rows the manual
     * DECLINES to answer (91emulator: "a refusal is not a check -- and an UNCOUNTED
     * refusal is not even a refusal").  FIRCCSR was one of 179 rows that were not
     * dropped, not refused, but INVISIBLE.
     *
     * THE DRIVER ANSWERS WHAT THE MANUAL WOULD NOT (fsl_clock.c:159-168):
     *
     *     SCG0->FIRCCSR |= SCG_FIRCCSR_FIRCEN_MASK;          // enable it
     *     while ((SCG0->FIRCCSR & SCG_FIRCCSR_FIRCVLD_MASK) == 0U) { }   // THEN wait
     *
     * It enables FIRC and only then spins on VLD.  A part whose FIRC is already valid
     * before anyone enabled it would make that spin meaningless.
     *
     *     ⭐ A HEADER IS A CLAIM ABOUT THE SILICON; A DRIVER IS A CLAIM ABOUT WHAT THE
     *        SILICON MUST DO.  WHEN THEY DISAGREE -- OR WHEN THE MANUAL IS SILENT --
     *        THE DRIVER WINS.                                    (rt1180emulator)
     *
     * So FIRC's VLD now FOLLOWS ITS ENABLE, like SOSC/ROSC/APLL/SPLL.  Firmware still
     * never spins: it is valid the instant it is enabled.  It is simply not valid
     * BEFORE.  (This is the FIFTH fabricated-ready in this tree, and the last one.)
     */
    case SCG_SIRCCSR:
        return v | SCG_READY;
    case SCG_FIRCCSR:
        return (v & FIRCCSR_FIRCEN) ? (v | SCG_READY) : v;
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

    /*
     * Re-derive and propagate whenever a register that feeds a source clock, a PLL,
     * or the main-clock select changes.  The source oscillators (SIRC/FIRC) plus the
     * PLL control/divider registers, the PLL power/enable, the external/32k enables,
     * and RCCR[SCS] all move the main clock.
     */
    switch (off) {
    case SCG_SIRCCSR:
    case SCG_FIRCCSR:
    case SCG_FIRCCFG:
    case SCG_SOSCCSR:
    case SCG_ROSCCSR:
    case SCG_RCCR:
    case SCG_APLLCSR: case SCG_APLLCTRL:
    case SCG_APLLNDIV: case SCG_APLLMDIV: case SCG_APLLPDIV: case SCG_APLLSSCG1:
    case SCG_SPLLCSR: case SCG_SPLLCTRL:
    case SCG_SPLLNDIV: case SCG_SPLLMDIV: case SCG_SPLLPDIV: case SCG_SPLLSSCG1:
        mcxn_scg_update_clocks(s);
        break;
    default:
        break;
    }
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

    /*
     * RESET VALUES, FROM THE RM's REGISTER MAP.  Every one of these was ZERO, and
     * zero is not a neutral default -- it is a CLAIM, and the SDK computes on it:
     *
     *   RCCR[SCS]      = 3 : the clock source the part is ACTUALLY running on out
     *                        of reset.  CLOCK_GetCoreSysClkFreq() switches on it;
     *                        0 is not a source, it is a hole.  (CSR mirrors RCCR.)
     *   xPLLNDIV/MDIV/PDIV = 1 : THE SDK DIVIDES BY THESE.  Zero is a divide-by-zero
     *                        sitting in the model waiting for someone to compute a
     *                        PLL frequency.
     *   xPLLLOCK_CNFG  = 0x4F4C, xPLLSSCG1 = 0x8000_0000, LDOCSR = 8 : read-modify-
     *                        written by the SDK's PLL setup; starting from 0 silently
     *                        drops bits the guest never knew it had.
     *   SIRCCSR        = 0x0100_0020 : bit 5 SIRC_CLK_PERIPH_EN is SET out of reset,
     *                        and without it CLOCK_GetFro12MFreq() returns 0 Hz and
     *                        EVERY FlexComm driver asserts sourceClock_Hz > 0 and
     *                        hard-faults.  That one bit is what started this audit.
     */
    s->regs[SCG_RCCR >> 2]           = 0x03000000u;   /* SCS = 3 */
    s->regs[SCG_SIRCCSR >> 2]        = SCG_SIRCCSR_RESET;
    /*
     * FIRC is ENABLED + VALID out of reset.  This was the latent inconsistency the
     * read-path comment above already named ("SIRC and FIRC genuinely run out of reset
     * and carry their VLD in their RESET VALUE") but the reset left FIRCCSR at 0.  It is
     * RM-DERIVED, not guessed: RCCR resets to SCS=3 (FRO_HF is the boot clock -- confirmed
     * in rm-golden.json, 0x0300_0000), so the FIRC MUST be running at reset, else the core
     * would boot with no clock; and the SDK's own clock_config explicitly sets
     * SCG_FIRCCSR_FIRCEN_CFG=Disabled for its FRO12M profile -- you disable what is on by
     * default.  So FRO_HF = 48 MHz out of reset (FIRCCFG[RANGE]=0), and the un-configured
     * core runs at 48 MHz, exactly as silicon does before BOARD_InitBootClocks.  (FIRCCSR
     * is not in the reset-values golden, so this changes no gated value.)
     */
    s->regs[SCG_FIRCCSR >> 2]        = FIRCCSR_FIRCEN | SCG_READY;
    s->regs[SCG_APLLNDIV >> 2]       = 0x00000001u;
    s->regs[SCG_APLLMDIV >> 2]       = 0x00000001u;
    s->regs[SCG_APLLPDIV >> 2]       = 0x00000001u;
    s->regs[SCG_APLLLOCK_CNFG >> 2]  = 0x00004F4Cu;
    s->regs[SCG_APLLSSCG1 >> 2]      = 0x80000000u;
    s->regs[SCG_SPLLNDIV >> 2]       = 0x00000001u;
    s->regs[SCG_SPLLMDIV >> 2]       = 0x00000001u;
    s->regs[SCG_SPLLPDIV >> 2]       = 0x00000001u;
    s->regs[SCG_SPLLLOCK_CNFG >> 2]  = 0x00004F4Cu;
    s->regs[SCG_SPLLSSCG1 >> 2]      = 0x80000000u;
    s->regs[SCG_LDOCSR >> 2]         = 0x00000008u;

    mcxn_scg_update_clocks(s);
}

static void mcxn_scg_realize(DeviceState *dev, Error **errp)
{
    MCXNSCGState *s = MCXN_SCG(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_scg_ops, s,
                          TYPE_MCXN_SCG, MCXN_SCG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    s->fro12m = qdev_init_clock_out(dev, "fro12m");
    s->frohf  = qdev_init_clock_out(dev, "frohf");
    s->apll   = qdev_init_clock_out(dev, "apll");
    s->spll   = qdev_init_clock_out(dev, "spll");
    s->mainclk = qdev_init_clock_out(dev, "mainclk");
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
