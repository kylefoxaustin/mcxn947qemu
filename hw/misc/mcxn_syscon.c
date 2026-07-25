/*
 * NXP MCX N SYSCON — secondary-core (CPU1) boot control (partial model)
 *
 * On the dual-Cortex-M33 MCXN947, the primary core (cpu0) releases the
 * secondary core (cpu1) by writing its vector-table base to SYSCON.CPBOOT and
 * enabling it via SYSCON.CPUCTRL (CPU1CLKEN set, CPU1RSTEN cleared).  This
 * device models exactly that handover and backs the remaining SYSCON registers
 * permissively so early firmware clock/reset init does not fault.
 *
 * Register offsets and bit fields are from the MCXN947 CMSIS header
 * (devices/MCXN947/MCXN947_cm33_core0.h): SYSCON CPUCTRL @0x800, CPBOOT @0x804.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/misc/mcxn_syscon.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "migration/vmstate.h"

/*
 * The OSTIMER's clock, per the SDK's own selector (fsl_clock.c):
 *
 *     uint32_t CLOCK_GetOstimerClkFreq(void) {
 *         switch (SYSCON->OSTIMERCLKSEL) {
 *             case 0U: freq = CLOCK_GetClk16KFreq(...);  break;   -- 16 000 Hz
 *             case 1U: freq = CLOCK_GetOsc32KFreq(...);  break;   -- 32 768 Hz
 *             case 2U: freq = CLOCK_GetClk1MFreq();      break;   --  1 000 000 Hz
 *             default: freq = 0U;                        break;   -- NO CLOCK
 *         }
 *     }
 *
 * OSTIMERCLKSEL RESETS TO 3, i.e. DEFAULT, i.e. NO CLOCK SELECTED.  A timer with no
 * clock DOES NOT RUN, and saying so is the whole point: the model used to hardcode
 * 1 MHz and ignore this register completely, so a guest selecting the 16 kHz source
 * got a timer 62x too fast and nothing said a word.
 *
 * 16000 and 1000000 are the SDK's own constants, not ours.  32768 is the FRDM board's
 * crystal.  Sources we do not model resolve to 0 Hz -- HONESTLY STOPPED, never
 * silently defaulted.
 */
#define SYSCON_OSTIMERCLKSEL  0x5E0
#define CLK16K_HZ    16000u      /* fsl_clock.c: CLOCK_GetClk16KFreq() */
#define OSC32K_HZ    32768u      /* FRDM-MCXN947 32.768 kHz crystal */
#define CLK1M_HZ     1000000u    /* fsl_clock.c: CLOCK_GetClk1MFreq() */

/*
 * CTIMERCLKSEL[5] @0x26C (step 4) and CTIMERCLKDIV[5] @0x3D0 (step 4), from CMSIS.
 *
 * ⚠ THESE ADDRESSES WERE READ OUT OF THE HEADER, NOT REMEMBERED.  Probing this bug I
 * first typed 0x40000570 and 0x40004000 from memory -- both WRONG -- and got a
 * confident, plausible, completely bogus measurement out of it.  A number you cannot
 * explain is not evidence.  GO AND READ THE ADDRESS.
 */
#define SYSCON_SCTCLKSEL      0x2F0   /* CMSIS SYSCON_Type */
#define SYSCON_SCTCLKDIV      0x3B4
#define SYSCON_CTIMERCLKSEL0  0x26C
#define SYSCON_CTIMERCLKDIV0  0x3D0
#define SYSCON_PLL1CLK0DIV    0x3E4   /* PLL1 clock-0 divider (CTIMER sel 2 / SCT sel 4) */
#define SYSCON_AHBCLKDIV      0x380   /* System (AHB/bus) clock divider: busclk = main/(N+1) */
#define SYSCON_SAI0CLKSEL     0x880
#define SYSCON_SAI1CLKSEL     0x884
#define SYSCON_SAI0CLKDIV     0x888
#define SYSCON_SAI1CLKDIV     0x88C
#define SAI_COUNT             2
#define CTIMER_COUNT          5
#define CLKDIV_DIV_MASK       0xFFu
#define CLKDIV_HALT           (1u << 30)

/* PLL1 output after its PLL1CLK0DIV divider (the divided PLL1 the CTIMER/SCT muxes see). */
static uint32_t mcxn_syscon_pll1_div(MCXNSysconState *s)
{
    uint32_t div = (s->regs[SYSCON_PLL1CLK0DIV / 4] & CLKDIV_DIV_MASK) + 1;

    return clock_get_hz(s->spll_in) / div;
}

/*
 * The CTIMER clock mux, mirroring the SDK's CLOCK_GetCTimerClkFreq() EXACTLY.
 *
 * IT MUST MIRROR IT EXACTLY, AND THAT IS THE WHOLE POINT.  The GUEST computes its
 * clock rate from these same registers and programs its match values from the answer.
 * If our timer ticks at a rate the driver did not compute, EVERY DELAY IT DERIVES IS
 * WRONG BY THAT RATIO, silently.  And it was: CTIMER ignored this selector entirely
 * and ran at sysclk, so firmware told "you are on FRO_HF at 48 MHz" got a timer
 * running at 150 MHz -- measured, with SysTick: 12011 ticks where the SDK's own
 * arithmetic expects 150000.
 *
 * Sources we can derive EXACTLY are derived.  The rest (PLL0/PLL1/SAI/...) are NOT
 * GUESSED: they resolve to 0 Hz and say so, loudly, once.  A stopped timer is a bug
 * you find in an hour; a timer running at a plausible wrong rate ships.
 */
static uint32_t mcxn_syscon_ctimer_src(MCXNSysconState *s, int n)
{
    uint32_t sel = s->regs[(SYSCON_CTIMERCLKSEL0 / 4) + n] & 0xFu;

    switch (sel) {
    case 0:  return CLK1M_HZ;                       /* CLOCK_GetClk1MFreq()  */
    case 1:  return clock_get_hz(s->apll_in);       /* CLOCK_GetPll0OutFreq()          */
    case 2:  return mcxn_syscon_pll1_div(s);        /* CLOCK_GetPll1OutFreq()/PLL1CLK0DIV */
    case 3:  return clock_get_hz(s->frohf_in);      /* CLOCK_GetFroHfFreq()  */
    case 4:  return clock_get_hz(s->fro12m_in);     /* CLOCK_GetFro12MFreq() */
    case 7:  return 0;                              /* none selected (reset) */
    default:
        qemu_log_mask(LOG_UNIMP,
            "mcxn-syscon: CTIMER%d clock source %u (SAI/LPOSC) is not "
            "modelled.  Reporting 0 Hz -- THE TIMER WILL NOT RUN -- rather than "
            "substituting a plausible rate, which would make every delay this "
            "driver computes silently wrong.\n", n, sel);
        return 0;
    }
}

/*
 * The SCT clock mux, mirroring CLOCK_GetSctClkFreq() (fsl_clock.c).  Every stock
 * example does CLOCK_AttachClk(kFRO_HF_to_SCT) -- selector 3, FRO_HF, 48 MHz -- and we
 * ticked the SCT at 150 MHz: 3.1x TOO FAST.
 */
static uint32_t mcxn_syscon_sct_src(MCXNSysconState *s)
{
    uint32_t sel = s->regs[SYSCON_SCTCLKSEL / 4] & 0x7u;

    switch (sel) {
    case 1:  return clock_get_hz(s->apll_in);       /* CLOCK_GetPll0OutFreq()          */
    case 3:  return clock_get_hz(s->frohf_in);      /* CLOCK_GetFroHfFreq() */
    case 4:  return mcxn_syscon_pll1_div(s);        /* CLOCK_GetPll1OutFreq()/PLL1CLK0DIV */
    case 0:
    case 7:  return 0;                              /* no source selected */
    default:
        qemu_log_mask(LOG_UNIMP,
            "mcxn-syscon: SCT clock source %u (ExtClk/SAI) is not modelled. "
            "Reporting 0 Hz -- THE SCT WILL NOT RUN -- rather than substituting a "
            "plausible rate, which would make every period it produces silently "
            "wrong.\n", sel);
        return 0;
    }
}

/*
 * The SAI function-clock (MCLK) mux, mirroring CLOCK_GetSaiClkFreq() (fsl_clock.c):
 *   1 = PLL0, 2 = ExtClk, 3 = FRO_HF, 4 = PLL1 / (PLL1CLK0DIV + 1).
 * The audio-friendly 12.288 MHz MCLK the old model hardcoded is one CONFIGURATION of this
 * (firmware sets PLL1 to an audio multiple and points SAI0CLKSEL at it); the model now
 * DERIVES whatever the guest selects.  ExtClk (an off-chip codec crystal) is a real source
 * but its rate is a board seam -- reported only if SOSC is enabled.  Unmodelled sources
 * (SAI MCLK-in loopbacks) report 0 loudly rather than a plausible wrong rate.
 */
static uint32_t mcxn_syscon_sai_src(MCXNSysconState *s, int n)
{
    hwaddr off = n ? SYSCON_SAI1CLKSEL : SYSCON_SAI0CLKSEL;
    uint32_t sel = s->regs[off / 4] & 0x7u;

    switch (sel) {
    case 1:  return clock_get_hz(s->apll_in);       /* CLOCK_GetPll0OutFreq()          */
    case 3:  return clock_get_hz(s->frohf_in);      /* CLOCK_GetFroHfFreq()            */
    case 4:  return mcxn_syscon_pll1_div(s);        /* CLOCK_GetPll1OutFreq()/PLL1CLK0DIV */
    case 0:
    case 7:  return 0;                              /* no source selected              */
    default:
        qemu_log_mask(LOG_UNIMP,
            "mcxn-syscon: SAI%d clock source %u (ExtClk/other) is not modelled. "
            "Reporting 0 Hz -- THE SAI HAS NO MCLK -- rather than a plausible wrong rate.\n",
            n, sel);
        return 0;
    }
}

static void mcxn_syscon_update_clocks(MCXNSysconState *s)
{
    uint32_t sel = s->regs[SYSCON_OSTIMERCLKSEL / 4] & 0x7u;
    uint32_t hz;
    int n;

    switch (sel) {
    case 0:  hz = CLK16K_HZ; break;
    case 1:  hz = OSC32K_HZ; break;
    case 2:  hz = CLK1M_HZ;  break;
    default: hz = 0;         break;   /* no source selected -- the timer STOPS */
    }
    clock_update_hz(s->ostimer_clk, hz);

    for (n = 0; n < CTIMER_COUNT; n++) {
        uint32_t div = s->regs[(SYSCON_CTIMERCLKDIV0 / 4) + n];
        uint32_t src = mcxn_syscon_ctimer_src(s, n);

        /* CLKDIV[30] = HALT: the divider output is stopped. */
        if (div & CLKDIV_HALT) {
            src = 0;
        } else {
            src /= (div & CLKDIV_DIV_MASK) + 1;
        }
        clock_update_hz(s->ctimer_clk[n], src);
    }

    {
        uint32_t div = s->regs[SYSCON_SCTCLKDIV / 4];
        uint32_t src = mcxn_syscon_sct_src(s);

        if (div & CLKDIV_HALT) {
            src = 0;
        } else {
            src /= (div & CLKDIV_DIV_MASK) + 1;
        }
        clock_update_hz(s->sct_clk, src);
    }

    for (n = 0; n < SAI_COUNT; n++) {
        hwaddr divoff = n ? SYSCON_SAI1CLKDIV : SYSCON_SAI0CLKDIV;
        uint32_t div = s->regs[divoff / 4];
        uint32_t src = mcxn_syscon_sai_src(s, n);

        if (div & CLKDIV_HALT) {
            src = 0;
        } else {
            src /= (div & CLKDIV_DIV_MASK) + 1;
        }
        clock_update_hz(s->sai_clk[n], src);
    }

    /*
     * The AHB/bus clock = main clock / (AHBCLKDIV + 1).  This feeds the M33 core (cpuclk/
     * refclk), the MRT and the FlexPWM -- the blocks that run on the raw bus clock.  It
     * FOLLOWS both the main clock (PLL reconfigure) and an AHBCLKDIV write.  AHBCLKDIV resets
     * to 0 (divide-by-1), so out of reset busclk == mainclk.
     */
    {
        uint32_t ahbdiv = (s->regs[SYSCON_AHBCLKDIV / 4] & CLKDIV_DIV_MASK) + 1;

        clock_update_hz(s->busclk, clock_get_hz(s->mainclk_in) / ahbdiv);
    }
}
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

/* --- SYSCON register offsets (CMSIS) --------------------------------------- */
#define SYSCON_CPUCTRL  0x800   /* CPU Control for Multiple Processors */
#define SYSCON_CPBOOT   0x804   /* Coprocessor (CPU1) Boot Address     */
#define SYSCON_CPSTAT   0x808   /* CPU Status                          */

/*
 * Peripheral-reset and AHB-clock control each expose a value register plus
 * write-1-to-set and write-1-to-clear alias registers (4 instances, step 4).
 * Firmware writes the SET alias then polls the value register for the bit, so
 * the model must reflect SET/CLR into the value register.
 */
#define SYSCON_PRESETCTRL0    0x100
#define SYSCON_PRESETCTRLSET  0x120
#define SYSCON_PRESETCTRLCLR  0x140
#define SYSCON_AHBCLKCTRL0    0x200
#define SYSCON_AHBCLKCTRLSET  0x220
#define SYSCON_AHBCLKCTRLCLR  0x240
#define SYSCON_CTRL_COUNT     4      /* instances of each control group */

/* True if [base, base + 4*count) contains offset (word-aligned). */
static inline bool in_range(hwaddr off, uint32_t base, uint32_t count)
{
    return off >= base && off < base + 4 * count;
}

/* --- CPUCTRL fields (CMSIS) ------------------------------------------------ */
#define CPUCTRL_CPU1CLKEN  (1u << 3)   /* SYSCON_CPUCTRL_CPU1CLKEN_MASK 0x8  */
#define CPUCTRL_CPU1RSTEN  (1u << 5)   /* SYSCON_CPUCTRL_CPU1RSTEN_MASK 0x20 */

/* CPBOOT holds the CPU1 vector-table (VTOR) base in bits [31:7]. */
#define CPBOOT_ADDR_MASK   0xFFFFFF80u

/* CPSTAT status bits (CMSIS): CPU1 sleeping. */
#define CPSTAT_CPU1SLEEPING (1u << 1)

/*
 * Keep cs->halted, the PSCI power_state and env->halt_reason consistent at
 * every start/stop.  After the WFI/WFE halt-reason rework in the QEMU base,
 * arm_cpu_has_work() asserts that a PSCI_OFF core is HALT_PSCI, so a running
 * core left at PSCI_OFF trips that assert on its first WFI.
 */
static void mcxn_syscon_set_cpu1_run(ARMCPU *cpu, bool run)
{
    CPUState *cs = CPU(cpu);

    cs->halted = !run;
    cpu->power_state = run ? PSCI_ON : PSCI_OFF;
    cpu->env.halt_reason = run ? NOT_HALTED : HALT_PSCI;
}

static void mcxn_syscon_start_cpu1_bh(void *opaque)
{
    MCXNSysconState *s = opaque;
    ARMCPU *cpu = s->cpu1;

    if (!cpu) {
        return;
    }
    /*
     * Boot CPU1 from the vector table SYSCON.CPBOOT points at: set its
     * init-SVTOR, reset so M-profile reload reads SP/PC from that table, then
     * release it.  Deferred to a BH because a store from cpu0's TCG block must
     * not retune another vCPU inline.
     */
    cpu->init_svtor = s->cpboot & CPBOOT_ADDR_MASK;
    cpu_reset(CPU(cpu));
    mcxn_syscon_set_cpu1_run(cpu, true);
    cpu_resume(CPU(cpu));
    s->cpu1_running = true;
}

static void mcxn_syscon_update_cpu1(MCXNSysconState *s)
{
    bool want_run = (s->cpuctrl & CPUCTRL_CPU1CLKEN) &&
                    !(s->cpuctrl & CPUCTRL_CPU1RSTEN);

    if (!s->cpu1) {
        return;
    }
    if (want_run && !s->cpu1_running) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                mcxn_syscon_start_cpu1_bh, s);
    } else if (!want_run && s->cpu1_running) {
        mcxn_syscon_set_cpu1_run(s->cpu1, false);
        s->cpu1_running = false;
    }
}

static uint64_t mcxn_syscon_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSysconState *s = MCXN_SYSCON(opaque);

    switch (offset) {
    case SYSCON_CPUCTRL:
        return s->cpuctrl;
    case SYSCON_CPBOOT:
        return s->cpboot;
    case SYSCON_CPSTAT:
        return s->cpu1_running ? 0 : CPSTAT_CPU1SLEEPING;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_syscon_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MCXNSysconState *s = MCXN_SYSCON(opaque);

    uint32_t v = value;

    /* SET/CLR alias registers reflect into their value register. */
    if (in_range(offset, SYSCON_PRESETCTRLSET, SYSCON_CTRL_COUNT)) {
        s->regs[(SYSCON_PRESETCTRL0 / 4) + (offset - SYSCON_PRESETCTRLSET) / 4] |= v;
        return;
    }
    if (in_range(offset, SYSCON_PRESETCTRLCLR, SYSCON_CTRL_COUNT)) {
        s->regs[(SYSCON_PRESETCTRL0 / 4) + (offset - SYSCON_PRESETCTRLCLR) / 4] &= ~v;
        return;
    }
    if (in_range(offset, SYSCON_AHBCLKCTRLSET, SYSCON_CTRL_COUNT)) {
        s->regs[(SYSCON_AHBCLKCTRL0 / 4) + (offset - SYSCON_AHBCLKCTRLSET) / 4] |= v;
        return;
    }
    if (in_range(offset, SYSCON_AHBCLKCTRLCLR, SYSCON_CTRL_COUNT)) {
        s->regs[(SYSCON_AHBCLKCTRL0 / 4) + (offset - SYSCON_AHBCLKCTRLCLR) / 4] &= ~v;
        return;
    }

    switch (offset) {
    case SYSCON_CPUCTRL:
        /* The PROT key in [31:16] gates writes on HW; modelled permissively. */
        s->cpuctrl = value;
        mcxn_syscon_update_cpu1(s);
        break;
    case SYSCON_CPBOOT:
        s->cpboot = value;
        break;
    default:
        s->regs[offset / 4] = value;
        /* Any selector or divider CHANGES A PERIPHERAL'S ACTUAL RATE. */
        if (offset == SYSCON_OSTIMERCLKSEL ||
            offset == SYSCON_SCTCLKSEL || offset == SYSCON_SCTCLKDIV ||
            offset == SYSCON_PLL1CLK0DIV ||
            offset == SYSCON_SAI0CLKSEL || offset == SYSCON_SAI1CLKSEL ||
            offset == SYSCON_SAI0CLKDIV || offset == SYSCON_SAI1CLKDIV ||
            offset == SYSCON_AHBCLKDIV ||
            (offset >= SYSCON_CTIMERCLKSEL0 &&
             offset <  SYSCON_CTIMERCLKSEL0 + 4 * CTIMER_COUNT) ||
            (offset >= SYSCON_CTIMERCLKDIV0 &&
             offset <  SYSCON_CTIMERCLKDIV0 + 4 * CTIMER_COUNT)) {
            mcxn_syscon_update_clocks(s);   /* the selector DECIDES THE RATE */
        }
        break;
    }
}

static const MemoryRegionOps mcxn_syscon_ops = {
    .read = mcxn_syscon_read,
    .write = mcxn_syscon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/*
 * SYSCON reset values, TAKEN FROM THE RM'S REGISTER MAP.
 *
 * ⚠ A CLOCK SELECTOR THAT RESETS TO 0 IS NOT "UNCONFIGURED" -- IT NAMES A REAL
 * SOURCE, AND THE SDK WILL HAND OUT ITS FREQUENCY.
 *
 * Every *CLKSEL resets to 7, which means NO SOURCE SELECTED, and every *CLKDIV
 * resets with bit 30 (HALT) SET.  We reset them all to zero, and the SDK reads them:
 *
 *     uint32_t CLOCK_GetCTimerClkFreq(uint32_t id) {
 *         switch (SYSCON->CTIMERCLKSEL[id]) {
 *             case 0U: freq = CLOCK_GetClk1MFreq(); break;   <-- 1 MHz!
 *             ...
 *             default: freq = 0U; break;                     <-- what 7 gives
 *         }
 *         return freq / div;
 *     }
 *
 * So a guest that asks for the clock of a CTimer/WDT/OSTIMER/MICFIL it never
 * configured gets, on silicon, 0 Hz -- and its driver asserts or bails, AND THE
 * DEVELOPER FINDS OUT.  On this model it got a PLAUSIBLE, WRONG, NON-ZERO frequency
 * (1 MHz, 16 kHz, 12 MHz), computed a bogus divider from it, and RAN THE TIMER AT
 * THE WRONG RATE, SILENTLY.
 *
 * That is worse than the SIRCCSR bug that started this audit.  THAT one hard-faulted,
 * which is loud.  THIS one returns a confident wrong number.
 *
 * CPUCTRL (reset 0x28 = CPU1CLKEN | CPU1RSTEN: clocked, but held in reset) also
 * mattered: firmware that only does `CPUCTRL &= ~CPU1RSTEN`, relying on CLKEN being
 * SET OUT OF RESET as it is on silicon, WOULD NEVER HAVE STARTED CPU1 here.
 *
 * Generated from the reference manual by the same extractor that backs
 * tests/mcxn-reset-values, so it cannot drift into invention.
 */
static const struct { uint16_t off; uint32_t val; } syscon_reset[] = {
    { 0x200, 0x00000603u },   /* AHBCLKCTRL0 */
    { 0x260, 0x00000007u },   /* SYSTICKCLKSEL0 */
    { 0x300, 0x40000000u },   /* SYSTICKCLKDIV0 */
    { 0x304, 0x40000000u },   /* SYSTICKCLKDIV1 */
    { 0x308, 0x40000000u },   /* TRACECLKDIV */
    { 0x37C, 0x40000000u },   /* TSICLKDIV */
    { 0x384, 0x40000000u },   /* CLKOUTDIV */
    { 0x388, 0x40000000u },   /* FROHFDIV */
    { 0x398, 0x40000000u },   /* USB0CLKDIV */
    { 0x3B4, 0x40000000u },   /* SCTCLKDIV */
    { 0x3C4, 0x40000000u },   /* PLLCLKDIV */
    { 0x3E4, 0x40000000u },   /* PLL1CLK0DIV */
    { 0x3E8, 0x40000000u },   /* PLL1CLK1DIV */
    { 0x400, 0x00020410u },   /* NVM_CTRL */
    { 0x490, 0x00000007u },   /* DAC0CLKSEL -- "none" (RM reset 0x7) */
    { 0x494, 0x40000000u },   /* DAC0CLKDIV -- HALT (RM reset 0x4000_0000) */
    { 0x498, 0x00000007u },   /* DAC1CLKSEL */
    { 0x49C, 0x40000000u },   /* DAC1CLKDIV */
    { 0x4A0, 0x00000007u },   /* DAC2CLKSEL */
    { 0x4A4, 0x40000000u },   /* DAC2CLKDIV */
    { 0x52C, 0x00000007u },   /* PLLCLKDIVSEL */
    { 0x530, 0x00000007u },   /* I3C0FCLKSEL */
    { 0x534, 0x00000007u },   /* I3C0FCLKSTCSEL */
    { 0x538, 0x40000000u },   /* I3C0FCLKSTCDIV */
    { 0x53C, 0x40000000u },   /* I3C0FCLKSDIV */
    { 0x540, 0x40000000u },   /* I3C0FCLKDIV */
    { 0x548, 0x0000000Fu },   /* MICFILFCLKSEL */
    { 0x54C, 0x40000000u },   /* MICFILFCLKDIV */
    { 0x560, 0x00000007u },   /* FLEXIOCLKSEL */
    { 0x564, 0x40000000u },   /* FLEXIOCLKDIV */
    { 0x5A0, 0x00000007u },   /* FLEXCAN0CLKSEL */
    { 0x5A8, 0x00000007u },   /* FLEXCAN1CLKSEL */
    { 0x5AC, 0x40000000u },   /* FLEXCAN1CLKDIV */
    { 0x5B0, 0x00000007u },   /* ENETRMIICLKSEL */
    { 0x5B4, 0x40000000u },   /* ENETRMIICLKDIV */
    { 0x5B8, 0x00000007u },   /* ENETPTPREFCLKSEL */
    { 0x5BC, 0x40000000u },   /* ENETPTPREFCLKDIV */
    { 0x5D4, 0x00000001u },   /* EWM0CLKSEL */
    { 0x5D8, 0x00000003u },   /* WDT1CLKSEL */
    { 0x5DC, 0x40000000u },   /* WDT1CLKDIV */
    { 0x5E0, 0x00000003u },   /* OSTIMERCLKSEL */
    { 0x5F0, 0x00000007u },   /* CMP0FCLKSEL */
    { 0x5F4, 0x40000000u },   /* CMP0FCLKDIV */
    { 0x5F8, 0x00000007u },   /* CMP0RRCLKSEL */
    { 0x5FC, 0x40000000u },   /* CMP0RRCLKDIV */
    { 0x600, 0x00000007u },   /* CMP1FCLKSEL */
    { 0x604, 0x40000000u },   /* CMP1FCLKDIV */
    { 0x608, 0x00000007u },   /* CMP1RRCLKSEL */
    { 0x60C, 0x40000000u },   /* CMP1RRCLKDIV */
    { 0x610, 0x00000007u },   /* CMP2FCLKSEL */
    { 0x614, 0x40000000u },   /* CMP2FCLKDIV */
    { 0x618, 0x00000007u },   /* CMP2RRCLKSEL */
    { 0x61C, 0x40000000u },   /* CMP2RRCLKDIV */
    { 0x800, 0x00000028u },   /* CPUCTRL */
    { 0x824, 0x00000031u },   /* LPCAC_CTRL */
    { 0x880, 0x00000007u },   /* SAI0CLKSEL */
    { 0x884, 0x00000007u },   /* SAI1CLKSEL */
    { 0x888, 0x40000000u },   /* SAI0CLKDIV */
    { 0x88C, 0x40000000u },   /* SAI1CLKDIV */
    { 0x890, 0x00000007u },   /* EMVSIM0CLKSEL */
    { 0x894, 0x00000007u },   /* EMVSIM1CLKSEL */
    { 0x898, 0x40000000u },   /* EMVSIM0CLKDIV */
    { 0x89C, 0x40000000u },   /* EMVSIM1CLKDIV */
    { 0xB30, 0x00000007u },   /* I3C1FCLKSEL */
    { 0xB34, 0x00000007u },   /* I3C1FCLKSTCSEL */
    { 0xB38, 0x40000000u },   /* I3C1FCLKSTCDIV */
    { 0xB3C, 0x40000000u },   /* I3C1FCLKSDIV */
    { 0xB40, 0x40000000u },   /* I3C1FCLKDIV */
    { 0xE04, 0x0000FFFFu },   /* AUTOCLKGATEOVERRIDE */
    { 0xE44, 0x00000003u },   /* ECC_ENABLE_CTRL */
};

static void mcxn_syscon_reset(DeviceState *dev)
{
    MCXNSysconState *s = MCXN_SYSCON(dev);
    int i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < (int)ARRAY_SIZE(syscon_reset); i++) {
        s->regs[syscon_reset[i].off / 4] = syscon_reset[i].val;
    }

    /*
     * CTIMERCLKSEL's RESET VALUE IS "u" -- UNDEFINED -- IN THE RM.  (Its reset column
     * literally says "See section", and the section's diagram shows u bits.  The
     * extractor behind tests/mcxn-reset-values SKIPPED this register rather than
     * invent a value for it, which is exactly what it should have done.)
     *
     * So this is a CHOICE, not a lookup, and the guardrail decides it:
     *
     *   reset to 0 (= clk1M)  -> firmware that FORGETS CLOCK_AttachClk() gets a
     *                            working timer HERE and undefined behaviour on
     *                            SILICON.  MORE PERMISSIVE THAN THE PART: the bug
     *                            passes here and ships.
     *   reset to 7 (= "No clock", the RM's own encoding: "111b - No clock")
     *                         -> that firmware gets a STOPPED TIMER, immediately,
     *                            loudly, and fixes it in an hour.
     *
     *     ⭐ WHERE SILICON IS UNDEFINED, PICK THE VALUE THAT EXPOSES THE GUEST'S
     *        MISTAKE, NOT THE ONE THAT HIDES IT.
     *
     * Every stock example calls CLOCK_AttachClk(kFRO_HF_to_CTIMERn) before using a
     * CTIMER.  Firmware that does not is relying on a value the RM does not promise.
     */
    for (i = 0; i < CTIMER_COUNT; i++) {
        s->regs[(SYSCON_CTIMERCLKSEL0 / 4) + i] = 7;   /* "No clock" */
    }

    mcxn_syscon_update_clocks(s);   /* OSTIMERCLKSEL resets to 3 = NO CLOCK */

    /* CPUCTRL lives in its own field (the CPU1 release path reads it), so the table
     * above cannot reach it.  CPU1CLKEN|CPU1RSTEN = clocked but held in reset, which
     * still evaluates to want_run == false.  See the comment above. */
    s->cpuctrl = 0x28;
    s->cpboot = 0;
    s->cpu1_running = false;
}

static void mcxn_syscon_realize(DeviceState *dev, Error **errp)
{
    MCXNSysconState *s = MCXN_SYSCON(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_syscon_ops, s,
                          TYPE_MCXN_SYSCON, MCXN_SYSCON_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    s->ostimer_clk = qdev_init_clock_out(dev, "ostimer-clk");
    {
        int n;

        for (n = 0; n < CTIMER_COUNT; n++) {
            g_autofree char *nm = g_strdup_printf("ctimer%d-clk", n);
            s->ctimer_clk[n] = qdev_init_clock_out(dev, nm);
        }
    }
    s->sct_clk = qdev_init_clock_out(dev, "sct-clk");
    {
        int n;

        for (n = 0; n < SAI_COUNT; n++) {
            g_autofree char *nm = g_strdup_printf("sai%d-clk", n);
            s->sai_clk[n] = qdev_init_clock_out(dev, nm);
        }
    }
    s->busclk = qdev_init_clock_out(dev, "busclk");
}

static const VMStateDescription vmstate_mcxn_syscon = {
    .name = TYPE_MCXN_SYSCON,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSysconState, MCXN_SYSCON_SIZE / 4),
        VMSTATE_UINT32(cpuctrl, MCXNSysconState),
        VMSTATE_UINT32(cpboot, MCXNSysconState),
        VMSTATE_BOOL(cpu1_running, MCXNSysconState),
        VMSTATE_END_OF_LIST()
    },
};

/* cpu1 is added as a settable link in instance_init (see there), not a DEFINE_PROP_LINK,
 * so the SoC can set it AFTER SYSCON realizes -- required to break the busclk<->cpu1
 * ordering cycle. */

static void mcxn_syscon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_syscon_realize;
    device_class_set_legacy_reset(dc, mcxn_syscon_reset);
    dc->vmsd = &vmstate_mcxn_syscon;
}

/* A source clock's rate changed upstream in SCG -- re-derive every peripheral's. */
static void mcxn_syscon_src_changed(void *opaque, ClockEvent event)
{
    mcxn_syscon_update_clocks(MCXN_SYSCON(opaque));
}

static void mcxn_syscon_init(Object *obj)
{
    MCXNSysconState *s = MCXN_SYSCON(obj);

    /*
     * Clock INPUTS must exist before anything can connect to them, so they are
     * created here in instance_init -- not in realize.  (And the connect itself must
     * happen BEFORE the target is realized: qdev_connect_clock_in() asserts
     * !dev->realized, the MIRROR IMAGE of the GPIO rule.)
     */
    s->fro12m_in = qdev_init_clock_in(DEVICE(obj), "fro12m",
                                      mcxn_syscon_src_changed, s, ClockUpdate);
    s->frohf_in  = qdev_init_clock_in(DEVICE(obj), "frohf",
                                      mcxn_syscon_src_changed, s, ClockUpdate);
    s->apll_in   = qdev_init_clock_in(DEVICE(obj), "apll",
                                      mcxn_syscon_src_changed, s, ClockUpdate);
    s->spll_in   = qdev_init_clock_in(DEVICE(obj), "spll",
                                      mcxn_syscon_src_changed, s, ClockUpdate);
    /* The SCG main clock -- divided by AHBCLKDIV into busclk.  A main-clock change (PLL
     * reconfigure) must re-derive busclk, so it carries the same update callback. */
    s->mainclk_in = qdev_init_clock_in(DEVICE(obj), "mainclk",
                                       mcxn_syscon_src_changed, s, ClockUpdate);
}

static const TypeInfo mcxn_syscon_types[] = {
    {
        .name          = TYPE_MCXN_SYSCON,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSysconState),
        .instance_init = mcxn_syscon_init,
        .class_init    = mcxn_syscon_class_init,
    },
};

DEFINE_TYPES(mcxn_syscon_types)
