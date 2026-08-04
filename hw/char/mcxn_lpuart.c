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
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"

/* --- LPUART core register offsets ---------------------------------- */
#define LPUART_VERID    0x00  /* RO */
#define LPUART_PARAM    0x04  /* RO */
#define LPUART_GLOBAL   0x08
#define LPUART_PINCFG   0x0C
#define LPUART_MCR      0x40   /*
                                * Modem Control -- the STOCK EDMA driver reads
                                * AND writes this; it was unhandled and showed
                                * up only when a REAL driver was run against the
                                * model, never in firmware I wrote myself.
                                */
#define LPUART_MSR      0x44   /* Modem Status */
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

/* --- LP_FLEXCOMM wrapper register offsets ------------------------ */
#define LPFLEXCOMM_ISTAT   0xFF4  /* RO */
#define LPFLEXCOMM_PSELID  0xFF8

/*
 * ⚠ LPI2C LIVES AT +0x800 INSIDE THE FLEXCOMM WINDOW.  LPUART AND LPSPI
 * DO NOT.
 *
 * CMSIS is unambiguous, and the three do not share a base:
 *     LP_FLEXCOMM0_BASE = 0x4009_2000
 *     LPUART0_BASE      = 0x4009_2000    (+0x000)
 *     LPSPI0_BASE       = 0x4009_2000    (+0x000)   -- overlays LPUART
 *     LPI2C0_BASE       = 0x4009_2800    (+0x800)   -- DOES NOT
 *
 * This decode had LPI2C at +0x000 alongside the other two.  So every
 * LPI2C register access from real firmware -- which naturally uses
 * LPI2C0_BASE -- landed at window offset 0x8xx, MATCHED NO CASE, AND WAS
 * SILENTLY DROPPED: reads returned 0, writes went nowhere.  THE WHOLE IP
 * WAS UNREACHABLE FROM THE GUEST.  The stock lpi2c examples print their
 * banner and then quietly do nothing, forever, because
 * LPI2C_MasterInit()'s every write fell on the floor.
 *
 * And my own LPI2C tests passed the entire time, because they poked THE
 * OFFSETS THIS FILE INVENTED.  The test agreed with the model because the
 * test got its address from the model.  That is not a test, it is a mirror
 * -- and no amount of mutation testing can see it, because mutating the
 * model moves the mirror too.  It took an INDEPENDENT golden (the RM's
 * reset values, read back at the addresses CMSIS gives) to notice that
 * nobody was home.
 */
#define LPI2C_WINDOW  0x800

/*
 * Reset values from the RM's register map.  Each of these was ZERO, and
 * zero is a CLAIM the guest acts on -- see the SCG SIRCCSR bug that
 * started this audit.
 */
/* OSR=15, SBR=4 -- the SDK DIVIDES by OSR */
#define LPUART_BAUD_RESET   0x0F000004u
#define LPUART_TOSR_RESET   0x0000000Fu
#define LPSPI_TCR_RESET     0x0000001Fu  /* FRAMESZ = 31 -> a 32-bit frame */

/*
 * FIFO[RXFIFOSIZE] (bits 2:0) and FIFO[TXFIFOSIZE] (bits 6:4) are READ-ONLY
 * CAPABILITY fields: the part telling software how deep its FIFOs are.
 * Reset 0x22 = size code 2 on each.  They were 0 (= the smallest FIFO),
 * and worse, they were STORED -- so the SDK's `base->FIFO = ...` during
 * init would have OVERWRITTEN the part's own description of itself.  A
 * capability register that software can change is not a capability
 * register.
 */
#define LPUART_FIFO_SIZES      0x00000022u
#define LPUART_FIFO_SIZES_MASK 0x00000077u
#define LPUART_DATA_RXEMPT     0x00001000u  /* CMSIS LPUART_DATA_RXEMPT_MASK */

/* --- Bit masks (CMSIS) --------------------------------------------- */
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
 * on UARTPRESENT via LP_FLEXCOMM_PeripheralIsPresent() — without it the
 * console driver bails before programming BAUD/CTRL and PRINTF silently
 * emits nothing.
 */
#define PSELID_UARTPRESENT  0x10u
#define PSELID_SPIPRESENT   0x20u
#define PSELID_I2CPRESENT   0x40u
#define PSELID_PRESENT_BITS \
    (PSELID_UARTPRESENT | PSELID_SPIPRESENT | PSELID_I2CPRESENT)

/*
 * PSELID[ID] (bits 31:12) -- the block identifying itself.  RM reset
 * for the whole register is 0x0010_3070: the three PRESENT bits (0x70),
 * the ID field (0x103000), and PERSEL = 0 (NO FUNCTION SELECTED).
 *
 * We returned 0x71: the present bits, NO ID AT ALL, and PERSEL = 1
 * (LPUART already chosen).  Both halves were wrong -- the part
 * under-reported what it is, and it claimed a function selection the
 * guest had not made.
 */
#define PSELID_ID_VALUE  0x00103000u

/*
 * VERID/PARAM are read by some HALs to size the FIFO.  PARAM is
 * DERIVED from the modelled FIFO depth (see below) so the two cannot
 * contradict each other.
 * MCX-class constants; refine against the RM if a HAL ever depends on them.
 */
#define LPUART_VERID_VALUE  0x04010003u
/*
 * ⚠ THIS USED TO BE 0x0000_0404, WITH THE COMMENT "Values are plausible".
 *
 *   ⭐ "PLAUSIBLE" IS THE WORD YOU USE WHEN YOU MEAN FABRICATED.  It is in
 *     this tree's own guardrails, and I wrote the rule and then wrote the
 *     word.
 *
 * And once the RX FIFO became REAL (8 deep, per RM FIFO[RXFIFOSIZE]=010b
 * -> "010b - 8"), that fabrication became a CONTRADICTION: FIFO said 8,
 * PARAM said 4.
 *   ⭐ TWO REGISTERS THAT DESCRIBE ONE RESOURCE MUST NOT DISAGREE.
 *
 * THE RM IS SILENT ON LPUART PARAM.  It has no row in the register
 * summary and no field description anywhere in 3763 pages -- which is
 * why it never appeared in the reset-value golden, and why the
 * fabrication survived.  AND NO DRIVER READS IT: not the MCUXpresso
 * SDK, not Zephyr.  So there is no manual to quote and no driver to
 * ask.
 *
 * MY FIRST FIX WAS 0x0808 (a literal depth of 8), argued from CMSIS
 * making PARAM's fields EIGHT BITS WIDE -- "a 2^n code needs only 4
 * bits, so 8 bits must mean a literal".
 * ⚠ THAT ARGUMENT IS FALSE, AND THE RM SAYS SO: LPSPI's RXFIFO IS ALSO 8
 *   BITS (15:8) AND IS EXPLICITLY 2^n -- "the maximum number of words is
 *   2**RXFIFO".  I had built a derivation out of a coincidence and was
 *   one build from shipping it.
 *
 * All THREE FIFO-size fields the RM does document -- LPI2C's
 * MRXFIFO/MTXFIFO, LPSPI's RXFIFO/TXFIFO -- are 2^n.  But I cannot
 * PROVE LPUART's is, so I do not have to:
 *
 *      value    if 2^n (the family convention)        if a literal depth
 *      -----    ------------------------------        ------------------
 *      0x0808   2^8 = 256 -- A CATASTROPHIC OVER-      8  (correct)
 *               PROMISE: a 256-deep FIFO we do not have
 *      0x0303   8 -- correct, and AGREES WITH FIFO    3  (an UNDER-report of 8)
 *
 *   ⭐ I DO NOT NEED TO RESOLVE THE ENCODING.  I NEED THE VALUE WHOSE
 *     FAILURE MODE IS UNDER-REPORTING.  On a capability register,
 *     under-reporting is a model that promises less than the chip;
 *     OVER-reporting is A PROMISE THE EMULATOR MAKES ON THE SILICON'S
 *     BEHALF, and the guest will hold us to it.  (91emulator, who learned
 *     this when QEMU refused to boot rather than let them advertise
 *     hardware they had not built.)
 *
 * DERIVED, NOT WRITTEN DOWN: PARAM follows MCXN_LPUART_FIFO_DEPTH.  A
 * capability register that is a CONSTANT can drift from the thing it
 * describes; one COMPUTED FROM it cannot.
 */
/* 2^3 = 8 = MCXN_LPUART_FIFO_DEPTH; asserted below */
#define LPUART_PARAM_FIFO_EXP 3u
#define LPUART_PARAM_VALUE \
    ((LPUART_PARAM_FIFO_EXP << 8) | LPUART_PARAM_FIFO_EXP)

/*
 * ⚠ THIS GUARD WAS A MIRROR, AND A SIBLING HAD TO READ MY CODE TO SEE IT.
 *
 *   It used to be:  (1u << LPUART_PARAM_FIFO_EXP) != MCXN_LPUART_FIFO_DEPTH
 *
 *   That is MACRO versus MACRO -- TWO SPELLINGS OF ONE BELIEF, AGREEING
 *   WITH THEMSELVES.  It never looks at the array the bytes actually
 *   land in.  Respell rx_fifo[] with a literal and PARAM would go on
 *   advertising 8 words over a 4-word buffer, silently.
 *
 *     ⭐ ASSERT AGAINST THE THING THE BYTES LAND IN, NOT AGAINST THE NAME
 *       YOU GAVE ITS SIZE.                                     (95emulator)
 *
 *   I tested exactly that mutation and the build DID fail -- and I
 *   nearly filed the guard as sound.  IT WAS NOT MY ASSERTION THAT
 *   CAUGHT IT.  It was vmstate.h, on a pointer-type mismatch, because I
 *   happen to migrate this array with the same macro as its length.
 *   PURE LUCK, and if the FIFO were not in the vmstate the guard would
 *   be blind.  ⭐ A SCREEN IS NOT A VERDICT -- INCLUDING THE SCREEN THAT
 *   SAYS "CAUGHT".  The catch was real; the ATTRIBUTION was wrong, and
 *   only reading the error told me.
 *
 *   And it is `>` and not `!=` on purpose: the model may legitimately
 *   hold MORE than it ADVERTISES -- under-reporting is the safe
 *   direction -- and `!=` would forbid exactly the direction this tree
 *   has spent the day arguing FOR.  Over-advertising is the bug.
 */
QEMU_BUILD_BUG_ON((1u << LPUART_PARAM_FIFO_EXP) >
                  ARRAY_SIZE(((MCXNLPUARTState *)0)->rx_fifo));

/* PERSEL function selections (LP_FLEXCOMM_PERIPH_T, CMSIS enum). */
#define PERSEL_LPSPI    2u
#define PERSEL_LPI2C    3u

/* LP_FLEXCOMM ISTAT (0xFF4) per-function interrupt-pending bits (CMSIS). */
#define ISTAT_UARTTX    0x1u
#define ISTAT_UARTRX    0x2u
#define ISTAT_SPI       0x4u
#define ISTAT_I2CM      0x10u

/* === LPSPI register offsets (PERSEL = 2, master view) ================== */
#define LPSPI_VERID     0x00  /* RO */
#define LPSPI_PARAM     0x04  /* RO */
#define LPSPI_CR        0x10
#define LPSPI_SR        0x14
#define LPSPI_IER       0x18
#define LPSPI_DER       0x1C
#define LPSPI_CFGR0     0x20
#define LPSPI_CFGR1     0x24
#define LPSPI_CCR       0x40
#define LPSPI_CCR1      0x44
#define LPSPI_FCR       0x58
#define LPSPI_FSR       0x5C  /* RO */
#define LPSPI_TCR       0x60
#define LPSPI_TDR       0x64  /* WO */
#define LPSPI_RSR       0x70  /* RO */
#define LPSPI_RDR       0x74  /* RO */
#define LPSPI_RDROR     0x78  /* RO */

#define LPSPI_CR_MEN    0x1u
#define LPSPI_CR_RST    0x2u
#define LPSPI_CR_RTF    0x100u  /* reset TX FIFO (self-clearing) */
#define LPSPI_CR_RRF    0x200u  /* reset RX FIFO (self-clearing) */
#define LPSPI_SR_TDF    0x1u
#define LPSPI_SR_RDF    0x2u
#define LPSPI_SR_WCF    0x100u
#define LPSPI_SR_FCF    0x200u
#define LPSPI_SR_TCF    0x400u
#define LPSPI_SR_W1C    (LPSPI_SR_WCF | LPSPI_SR_FCF | LPSPI_SR_TCF)
#define LPSPI_IER_TDIE  0x1u
#define LPSPI_IER_RDIE  0x2u
#define LPSPI_TCR_FRAMESZ 0xFFFu
#define LPSPI_TCR_RXMSK 0x80000u
/* continuous transfer: hold CS asserted across frames */
#define LPSPI_TCR_CONT  0x200000u
#define LPSPI_RSR_RXEMPTY 0x2u

#define LPSPI_VERID_VALUE  0x01010004u
/*
 * ⚠ THIS USED TO BE 0x0004_0404 -- "RX/TX FIFO depth exp=4", i.e.
 *   2^4 = SIXTEEN words.  THE MODEL HOLDS ONE BYTE (`spi_rx_full`).  We
 *   advertised a 16-deep FIFO and shipped a single register.
 *
 *   And the RM is explicit about the encoding here (unlike LPUART's
 *   PARAM, which it does not document at all):
 *
 *       RXFIFO: "Indicates the maximum number of words in the receive FIFO.
 *                The maximum number of words is 2**RXFIFO."
 *
 *   The SDK's idiom is LPSPI_GetTxFifoSize() = 1U << (PARAM &
 *   TXFIFO_MASK), and drivers push that many words before they bother
 *   to check TDF.  ⇒ A GUEST BELIEVING US WOULD PUSH SIXTEEN WORDS INTO
 *   A ONE-WORD REGISTER AND DROP FIFTEEN OF THEM, SILENTLY.
 *
 *   ⭐ UNDER-REPORTING IS A MODEL THAT PROMISES LESS THAN THE CHIP.
 *     OVER-REPORTING IS A PROMISE THE EMULATOR MAKES ON THE SILICON'S
 *     BEHALF -- AND THE GUEST WILL HOLD US TO IT.  So we now report what
 *     we actually DELIVER: 2^0 = 1 word.
 *
 *   This is a DECISION, not a gap: the real silicon has deeper FIFOs,
 *   and when this model grows them, this value must grow with them.
 *   Filed, with the reason, so the next person to look does not "fix"
 *   it back to the datasheet and re-arm the bug.
 *   (PCSNUM=4 is a pin count, not a FIFO promise, and is left alone.)
 */
/* PCSNUM=4; RX/TX FIFO exp=0 -> 1 word, as modelled */
#define LPSPI_PARAM_VALUE  0x00040000u

/* === LPI2C register offsets (PERSEL = 3, controller/master view) ========= */
#define LPI2C_VERID     0x00  /* RO */
#define LPI2C_PARAM     0x04  /* RO */
#define LPI2C_MCR       0x10
#define LPI2C_MSR       0x14
#define LPI2C_MIER      0x18
#define LPI2C_MDER      0x1C
#define LPI2C_MCFGR0    0x20
#define LPI2C_MCFGR1    0x24
#define LPI2C_MCFGR2    0x28
#define LPI2C_MCFGR3    0x2C
#define LPI2C_MCCR0     0x48
#define LPI2C_MCCR1     0x50
#define LPI2C_MFCR      0x58
#define LPI2C_MFSR      0x5C  /* RO */
#define LPI2C_MTDR      0x60  /* WO */
#define LPI2C_MRDR      0x70  /* RO */
#define LPI2C_MRDROR    0x78  /* RO -- NON-DESTRUCTIVE alias of MRDR */
#define LPI2C_SASR      0x150 /* RO -- slave address status */
#define LPI2C_SRDR      0x170 /* RO -- slave receive data */
#define LPI2C_SRDROR    0x178 /* RO -- NON-DESTRUCTIVE alias of SRDR */

#define LPI2C_MCR_MEN   0x1u
#define LPI2C_MCR_RST   0x2u
#define LPI2C_MCR_RTF   0x100u  /* reset TX FIFO (self-clearing) */
#define LPI2C_MCR_RRF   0x200u  /* reset RX FIFO (self-clearing) */
#define LPI2C_MSR_TDF   0x1u
#define LPI2C_MSR_RDF   0x2u
#define LPI2C_MSR_EPF   0x100u
#define LPI2C_MSR_SDF   0x200u
#define LPI2C_MSR_NDF   0x400u
#define LPI2C_MSR_MBF   0x1000000u
#define LPI2C_MSR_BBF   0x2000000u
#define LPI2C_MSR_W1C   (LPI2C_MSR_EPF | LPI2C_MSR_SDF | LPI2C_MSR_NDF)
#define LPI2C_MIER_TDIE 0x1u
#define LPI2C_MIER_RDIE 0x2u
#define LPI2C_MRDR_RXEMPTY 0x4000u
#define LPI2C_MTDR_CMD_SHIFT 8
#define LPI2C_MTDR_CMD_MASK  0x700u
#define LPI2C_MTDR_DATA_MASK 0xFFu
/* MTDR command field [10:8] (CMSIS RM): */
#define LPI2C_CMD_TXDATA   0u   /* transmit DATA byte                    */
#define LPI2C_CMD_RXDATA   1u   /* receive (DATA+1) bytes                */
#define LPI2C_CMD_STOP     2u   /* generate STOP                         */
/* generate (re)START + transmit address; 4..7 are START variants */
#define LPI2C_CMD_START    4u

#define LPI2C_VERID_VALUE  0x01000003u
/*
 * ⚠ THIS USED TO BE 0x0000_0202 -- 2^2 = FOUR words each way.  The model
 *   holds ONE byte (`i2c_rx_full`).  RM, explicitly: "Configures the
 *   number of words in the controller receive FIFO to 2**MRXFIFO."  Same
 *   over-promise as LPSPI, smaller blast radius.  Report what we deliver:
 *   2^0 = 1.  A DECISION, with its reason -- see LPSPI above.
 */
/* M TX/RX FIFO exp=0 -> 1 word, as modelled */
#define LPI2C_PARAM_VALUE  0x00000000u

/* Dynamic LPSPI status: latched W1C flags plus the always-current TDF/RDF. */
/*
 * ⭐ THE RX FIFO IS REAL NOW, AND THE WATERMARK IS WHY IT MATTERS.
 *
 * RM, on STAT[RDRF], verbatim:
 *   "This field becomes 1 when the number of datawords in the receive buffer is
 *    GREATER THAN the number [in WATER[RXWATER]]."
 *
 * RXWATER resets to 0, so RDRF degenerates to "any byte present" --
 * which is EXACTLY what a depth-1 holding register does.  That is why
 * a one-byte model passed 70 suites while advertising an 8-deep FIFO:
 * NOTHING WE TEST EVER SET A WATERMARK.  The capability was a promise
 * nobody had called in yet.
 *
 * FIFO[RXFE] gates the FIFO: with it clear the receiver is a single
 * dataword, which is what the hardware does too -- so the depth is
 * not a constant, it is a FUNCTION OF
 * THE GUEST'S OWN CONFIGURATION, and we must ask it rather than assume it.
 */
#define FIFO_RXFLUSH 0x00004000u   /* CMSIS LPUART_FIFO_RXFLUSH_MASK */
#define FIFO_TXFLUSH 0x00008000u   /* CMSIS LPUART_FIFO_TXFLUSH_MASK */
#define FIFO_RXEMPT  0x00400000u   /* CMSIS LPUART_FIFO_RXEMPT_MASK  */
#define FIFO_TXEMPT  0x00800000u   /* CMSIS LPUART_FIFO_TXEMPT_MASK  */

static inline unsigned lpuart_rx_depth(MCXNLPUARTState *s)
{
    return (s->fifo & FIFO_RXFE) ? MCXN_LPUART_FIFO_DEPTH : 1;
}

static inline unsigned lpuart_rxwater(MCXNLPUARTState *s)
{
    return (s->water >> 16) & 0x7;   /* CMSIS LPUART_WATER_RXWATER_MASK/SHIFT */
}

/*
 * RM: RDRF is a LEVEL -- "datawords in the receive buffer GREATER THAN
 * RXWATER".
 */
static inline bool lpuart_rdrf(MCXNLPUARTState *s)
{
    return s->rx_count > lpuart_rxwater(s);
}

static bool lpuart_rx_push(MCXNLPUARTState *s, uint8_t ch)
{
    if (s->rx_count >= lpuart_rx_depth(s)) {
        return false;             /* caller raises STAT[OR]; the byte is LOST */
    }
    s->rx_fifo[(s->rx_head + s->rx_count) % MCXN_LPUART_FIFO_DEPTH] = ch;
    s->rx_count++;
    return true;
}

static uint8_t lpuart_rx_pop(MCXNLPUARTState *s)
{
    uint8_t ch;

    if (!s->rx_count) {
        return 0;
    }
    ch = s->rx_fifo[s->rx_head];
    s->rx_head = (s->rx_head + 1) % MCXN_LPUART_FIFO_DEPTH;
    s->rx_count--;
    return ch;
}

static uint32_t mcxn_lpspi_status(MCXNLPUARTState *s)
{
    uint32_t sr = s->spi_sr;
    if (s->spi_cr & LPSPI_CR_MEN) {
        sr |= LPSPI_SR_TDF;          /* synchronous TX FIFO always has room */
    }
    if (s->spi_rx_full) {
        sr |= LPSPI_SR_RDF;
    }
    return sr;
}

/* Dynamic LPI2C controller status. */
static uint32_t mcxn_lpi2c_status(MCXNLPUARTState *s)
{
    uint32_t msr = s->i2c_msr;

    /*
     * TDF means "the TX FIFO is at or below its watermark", i.e. THERE
     * IS ROOM.  That is true of an empty FIFO whether or not the module
     * is enabled, which is why the RM gives MSR a reset value of 1 with
     * MEN still clear.  Gating it on MEN made TDF mean "enabled AND has
     * room" -- a different claim, and one that reads back 0 out of reset
     * where silicon reads 1.
     */
    msr |= LPI2C_MSR_TDF;            /* synchronous TX FIFO always has room */
    if (s->i2c_rx_full) {
        msr |= LPI2C_MSR_RDF;
    }
    if (s->i2c_busy) {
        msr |= LPI2C_MSR_MBF | LPI2C_MSR_BBF;
    }
    return msr;
}

/*
 * LP_FLEXCOMM ISTAT (0xFF4): which selected function currently has an enabled
 * interrupt pending.  The single FlexComm NVIC line is the OR of these; the
 * SDK's LP_FLEXCOMM IRQ handler reads ISTAT to dispatch to the sub-driver.
 */
static uint32_t mcxn_flexcomm_istat(MCXNLPUARTState *s)
{
    uint32_t istat = 0;

    switch (s->pselid & PSELID_PERSEL) {
    case PERSEL_LPSPI:
        if (s->spi_ier & mcxn_lpspi_status(s)) {
            istat |= ISTAT_SPI;
        }
        break;
    case PERSEL_LPI2C:
        if (s->i2c_mier & mcxn_lpi2c_status(s)) {
            istat |= ISTAT_I2CM;
        }
        break;
    default: /* LPUART (or NONE) */
        /*
         * TX data-register-empty and transmit-complete are always
         * asserted in this model (writes are synchronous), so TIE/TCIE
         * assert immediately.
         */
        if (s->ctrl & (CTRL_TIE | CTRL_TCIE)) {
            istat |= ISTAT_UARTTX;
        }
        if ((s->ctrl & CTRL_RIE) && lpuart_rdrf(s)) {
            istat |= ISTAT_UARTRX;
        }
        break;
    }
    return istat;
}

/* DMA-enable bits each stock driver sets (CMSIS). */
#define BAUD_RDMAE   0x00200000u   /* LPUART_BAUD[RDMAE] */
#define BAUD_TDMAE   0x00800000u   /* LPUART_BAUD[TDMAE] */
#define DER_TDDE     0x1u          /* LPSPI_DER / LPI2C_MDER [TDDE] */
#define DER_RDDE     0x2u          /* LPSPI_DER / LPI2C_MDER [RDDE] */

/*
 * THE DMA REQUEST LINES (LpFlexcomm{n} Rx = 69 + 2n, Tx = 70 + 2n — CMSIS
 * dma_request_source_t).
 *
 * ⚠ These did not exist, and their absence is the reason every stock
 * LPUART/LPSPI/LPI2C DMA driver — LPUART_TransferSendEDMA,
 * LPSPI_MasterTransferEDMA, LPI2C_MasterTransferEDMA — would have HUNG: the
 * driver arms an eDMA channel at the data register, sets the peripheral's
 * DMA-enable bit, and waits for a request that NOTHING COULD RAISE.
 *
 * I had flagged this gap in the docs ("wired for SAI and DAC only") and stopped
 * there.  rt1180emulator named the organ: **"An honestly-documented missing
 * capability is still a missing capability.  Naming a gap in the
 * place you first met it is not the same as understanding its extent.
 * The flag discharges the
 * anxiety and the gap stays."**  He flagged his in one row and it was a gap in
 * twelve; mine covered four families and the chip has 117 request sources.
 *
 * Level-driven and edge-suppressed: a qemu_irq handler runs on EVERY
 * qemu_set_irq call, and the eDMA re-enters us as it drains/fills.
 */
static void mcxn_flexcomm_update_dma(MCXNLPUARTState *s)
{
    bool tx = false, rx = false;

    switch (s->pselid & PSELID_PERSEL) {
    case 2:                                         /* LPSPI */
        tx = (s->spi_der & DER_TDDE) != 0;          /* TX FIFO always ready */
        rx = (s->spi_der & DER_RDDE) && s->spi_rx_full;
        break;
    case 3:                                         /* LPI2C */
        tx = (s->i2c_mder & DER_TDDE) != 0;
        rx = (s->i2c_mder & DER_RDDE) && s->i2c_rx_full;
        break;
    default:                                        /* LPUART */
        /*
         * TDRE is always asserted here (writes are synchronous), so an
         * armed TX DMA request is continuously asserted until the
         * channel's major loop completes and TCD_CSR[DREQ] clears ERQ —
         * which is exactly how a real UART TX DMA drains a buffer.
         */
        tx = (s->baud & BAUD_TDMAE) != 0;
        rx = (s->baud & BAUD_RDMAE) && lpuart_rdrf(s);
        break;
    }

    if (tx != s->dma_tx_level) {
        s->dma_tx_level = tx;
        qemu_set_irq(s->dma_req_tx, tx);
    }
    if (rx != s->dma_rx_level) {
        s->dma_rx_level = rx;
        qemu_set_irq(s->dma_req_rx, rx);
    }
}

static void mcxn_flexcomm_update_irq(MCXNLPUARTState *s)
{
    qemu_set_irq(s->irq, mcxn_flexcomm_istat(s) != 0);
    mcxn_flexcomm_update_dma(s);
}

/* === LPSPI (master) function ============================================== */
static uint64_t mcxn_lpspi_read(MCXNLPUARTState *s, hwaddr offset)
{
    switch (offset) {
    case LPSPI_VERID: return LPSPI_VERID_VALUE;
    case LPSPI_PARAM: return LPSPI_PARAM_VALUE;
    case LPSPI_CR:    return s->spi_cr;
    case LPSPI_SR:    return mcxn_lpspi_status(s);
    case LPSPI_IER:   return s->spi_ier;
    case LPSPI_DER:   return s->spi_der;
    case LPSPI_CFGR0: return s->spi_cfgr0;
    case LPSPI_CFGR1: return s->spi_cfgr1;
    case LPSPI_CCR:   return s->spi_ccr;
    case LPSPI_FCR:   return s->spi_fcr;
    case LPSPI_TCR:   return s->spi_tcr;
    case LPSPI_FSR:
        return s->spi_rx_full ? (1u << 16) : 0; /* RXCOUNT=1 */
    case LPSPI_RSR:
        return s->spi_rx_full ? 0 : LPSPI_RSR_RXEMPTY;
    case LPSPI_RDROR: return s->spi_rdr;                     /* peek, no pop */
    case LPSPI_RDR: {
        uint32_t v = s->spi_rdr;
        if (s->spi_rx_full) {
            s->spi_rx_full = false;
            mcxn_flexcomm_update_irq(s);
        }
        return v;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled LPSPI read @0x%03" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
}

static void mcxn_lpspi_write(MCXNLPUARTState *s, hwaddr offset, uint32_t value)
{
    switch (offset) {
    case LPSPI_CR:
        if (value & LPSPI_CR_RST) {
            s->spi_cr = s->spi_sr = s->spi_ier = 0;
            s->spi_rx_full = false;
            if (s->spi_cs_asserted) {     /* a reset releases the chip-select */
                qemu_set_irq(s->spi_cs, 1);
                s->spi_cs_asserted = false;
            }
        } else {
            s->spi_cr = value & ~(LPSPI_CR_RTF | LPSPI_CR_RRF);
            if (value & LPSPI_CR_RRF) {
                s->spi_rx_full = false;   /* flush RX FIFO */
            }
            /* RTF: TX FIFO is always empty in this synchronous model. */
        }
        mcxn_flexcomm_update_irq(s);
        break;
    case LPSPI_SR:
        s->spi_sr &= ~(value & LPSPI_SR_W1C);
        mcxn_flexcomm_update_irq(s);
        break;
    case LPSPI_IER:
        s->spi_ier = value;
        mcxn_flexcomm_update_irq(s);
        break;
    case LPSPI_CFGR0:
        s->spi_cfgr0 = value;
        break;
    case LPSPI_CFGR1:
        s->spi_cfgr1 = value;
        break;
    case LPSPI_CCR:
        s->spi_ccr = value;
        break;
    case LPSPI_FCR:
        s->spi_fcr = value;
        break;
    case LPSPI_TCR:
        s->spi_tcr = value;
        break;
    case LPSPI_CCR1:
        break;  /* accepted, not modelled */
    case LPSPI_DER:
        /*
         * The DMA-enable bits.  These used to be ACCEPTED AND DISCARDED, so
         * the stock LPSPI_MasterTransferEDMA driver armed a channel, set
         * TDDE, and waited forever for a request nothing could raise.
         */
        s->spi_der = value;
        mcxn_flexcomm_update_irq(s);
        break;
    case LPSPI_TDR:
        /*
         * Master transmit: shift one word out.  With no external device the
         * word loops straight back into the RX FIFO (a physical MOSI->MISO
         * jumper), so a self-contained master transfer completes and can read
         * its own data back — the same loopback idiom as the FlexCAN/I3C
         * models.  RXMSK in TCR suppresses the receive (write-only transfer).
         */
        if (s->spi_cr & LPSPI_CR_MEN) {
            if (!(s->spi_tcr & LPSPI_TCR_RXMSK)) {
                /* bits */
                uint32_t framesz = (s->spi_tcr & LPSPI_TCR_FRAMESZ) + 1;
                uint32_t mask = (framesz >= 32) ? 0xFFFFFFFFu
                                                : ((1u << framesz) - 1);
                if (s->spi_bus) {
                    /*
                     * Shift the word out over the SSI bus.  The chip-select
                     * is asserted (active low) before the first frame and
                     * held while TCR[CONT] is set, so a multi-byte command
                     * (e.g. m25p80 RDID: 0x9F then 3 ID bytes) is ONE
                     * transaction -- CS drops only when a frame clears CONT.
                     * For a board-to-board spi-link the CS line is unwired
                     * and this is a no-op.
                     */
                    if (!s->spi_cs_asserted) {
                        qemu_set_irq(s->spi_cs, 0);
                        s->spi_cs_asserted = true;
                    }
                    s->spi_rdr = ssi_transfer(s->spi_bus, value & mask) & mask;
                    if (!(s->spi_tcr & LPSPI_TCR_CONT)) {
                        qemu_set_irq(s->spi_cs, 1);
                        s->spi_cs_asserted = false;
                    }
                } else {
                    /* Self-contained loopback (MOSI->MISO jumper). */
                    s->spi_rdr = value & mask;
                }
                s->spi_rx_full = true;
            }
            s->spi_sr |= LPSPI_SR_WCF | LPSPI_SR_FCF | LPSPI_SR_TCF;
            mcxn_flexcomm_update_irq(s);
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled LPSPI write @0x%03" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, value);
        break;
    }
}

/* === LPI2C (controller/master) function =================================== */
static uint64_t mcxn_lpi2c_read(MCXNLPUARTState *s, hwaddr offset)
{
    switch (offset) {
    case LPI2C_VERID:  return LPI2C_VERID_VALUE;
    case LPI2C_PARAM:  return LPI2C_PARAM_VALUE;
    case LPI2C_MCR:    return s->i2c_mcr;
    case LPI2C_MSR:    return mcxn_lpi2c_status(s);
    case LPI2C_MIER:   return s->i2c_mier;
    case LPI2C_MCFGR1: return s->i2c_mcfgr1;
    case LPI2C_MFSR:
        return s->i2c_rx_full ? (1u << 16) : 0;  /* RXCOUNT=1 */
    /*
     * ⚠ MRDR WAS RIGHT AND ITS ALIAS WAS WRONG, WHICH IS THE WHOLE POINT.
     *
     * MRDROR is the NON-DESTRUCTIVE alias of MRDR -- a peek that does not
     * pop.  It was not modelled at all, so it fell through to the default
     * and RETURNED ZERO.  And zero is not "nothing": RXEMPTY is bit 14, so
     * zero means THE RECEIVE FIFO HAS DATA.
     *
     *     ⭐ AN UNMODELLED REGISTER IS NOT A FREE REGISTER.  IT STILL
     *        ANSWERS -- AND ZERO IS AN ANSWER.  (91emulator, who shipped
     *        the identical bug: their MRDR was correct and its alias was
     *        not.)
     *
     * A driver that polls the non-destructive alias -- exactly what an
     * alias is FOR -- saw RXEMPTY clear and read a PHANTOM BYTE out of an
     * empty FIFO.
     *
     * The slave registers (SASR/SRDR/SRDROR) are likewise unmodelled, and
     * likewise were answering "data available" to anyone who asked.  We
     * do not model the LPI2C slave engine, so they now report HONESTLY
     * EMPTY.  A missing feature that says "empty" is
     * a gap; a missing feature that says "here is a byte" is a lie.
     */
    case LPI2C_MRDROR:
        /* peek, no pop */
        return s->i2c_rx_full ? s->i2c_mrdr : LPI2C_MRDR_RXEMPTY;

    case LPI2C_SASR:
    case LPI2C_SRDR:
    case LPI2C_SRDROR:
        /* The slave engine is not modelled.  It is EMPTY, and it says so. */
        return LPI2C_MRDR_RXEMPTY;

    case LPI2C_MRDR: {
        uint32_t v;
        if (s->i2c_rx_full) {
            v = s->i2c_mrdr;
            /*
             * Refill the 1-byte lookahead from the bus if more bytes were
             * requested, so a multi-byte receive drains one MRDR read at a
             * time.
             */
            if (s->i2c_rx_pending) {
                s->i2c_mrdr = i2c_recv(s->i2c_bus);
                s->i2c_rx_pending--;
            } else {
                s->i2c_rx_full = false;
            }
            mcxn_flexcomm_update_irq(s);
        } else {
            v = LPI2C_MRDR_RXEMPTY;
        }
        return v;
    }
    case LPI2C_MCFGR0:
    case LPI2C_MCFGR2:
    case LPI2C_MCFGR3:
    case LPI2C_MCCR0:
    case LPI2C_MCCR1:
    case LPI2C_MFCR:
        return 0;  /* accepted, not modelled */
    case LPI2C_MDER:
        return s->i2c_mder;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled LPI2C read @0x%03" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
}

static void mcxn_lpi2c_write(MCXNLPUARTState *s, hwaddr offset, uint32_t value)
{
    switch (offset) {
    case LPI2C_MCR:
        if (value & LPI2C_MCR_RST) {
            s->i2c_mcr = s->i2c_msr = s->i2c_mier = 0;
            s->i2c_rx_full = s->i2c_busy = false;
            s->i2c_rx_pending = 0;
        } else {
            s->i2c_mcr = value & ~(LPI2C_MCR_RTF | LPI2C_MCR_RRF);
            if (value & LPI2C_MCR_RRF) {
                s->i2c_rx_full = false;
            }
        }
        mcxn_flexcomm_update_irq(s);
        break;
    case LPI2C_MSR:
        s->i2c_msr &= ~(value & LPI2C_MSR_W1C);
        mcxn_flexcomm_update_irq(s);
        break;
    case LPI2C_MIER:
        s->i2c_mier = value;
        mcxn_flexcomm_update_irq(s);
        break;
    case LPI2C_MCFGR1:
        s->i2c_mcfgr1 = value;
        break;
    case LPI2C_MCFGR0:
    case LPI2C_MCFGR2:
    case LPI2C_MCFGR3:
    case LPI2C_MCCR0:
    case LPI2C_MCCR1:
    case LPI2C_MFCR:
        break;  /* accepted, not modelled */
    case LPI2C_MDER:
        /* DMA-enable bits — see LPSPI_DER. */
        s->i2c_mder = value;
        mcxn_flexcomm_update_irq(s);
        break;
    case LPI2C_MTDR: {
        /*
         * Controller command engine, driving a REAL I2C bus (the test
         * attaches a genuine at24c EEPROM to it) -- NOT the echo target this
         * used to be.  The old model ACKed every address and returned the
         * last transmitted byte on a receive, so its own tests passed against
         * a fabricated peer (the uSDHC bug class: the model was its own
         * oracle).  Now:
         *   START+addr (cmd 4..7): DATA = (7-bit addr << 1) | R/W.
         *      i2c_start_transfer drives the bus; a device that does not ACK
         *      sets MSR[NDF] (a real bus error).
         *   TXDATA (cmd 0): i2c_send; a NACKed byte sets NDF.
         *   RXDATA (cmd 1): receive DATA+1 bytes (drained lazily by MRDR
         *      reads, so a multi-byte read needs no deep FIFO -- one lookahead
         *      byte + a pending count).
         *   STOP  (cmd 2): i2c_end_transfer + SDF/EPF.
         */
        uint32_t cmd = (value & LPI2C_MTDR_CMD_MASK) >> LPI2C_MTDR_CMD_SHIFT;
        uint8_t data = value & LPI2C_MTDR_DATA_MASK;

        if (!(s->i2c_mcr & LPI2C_MCR_MEN)) {
            break;
        }
        switch (cmd) {
        case LPI2C_CMD_START:
        case 5: case 6: case 7:    /* all (repeated) START + address variants */
            if (i2c_start_transfer(s->i2c_bus, data >> 1, data & 1)) {
                s->i2c_msr |= LPI2C_MSR_NDF;   /* no device ACKed the address */
                s->i2c_busy = false;
            } else {
                s->i2c_busy = true;
            }
            break;
        case LPI2C_CMD_TXDATA:
            if (i2c_send(s->i2c_bus, data)) {
                /* the addressed device NACKed the byte */
                s->i2c_msr |= LPI2C_MSR_NDF;
            }
            break;
        case LPI2C_CMD_RXDATA:
            s->i2c_rx_pending += (uint32_t)data + 1;  /* receive DATA+1 bytes */
            if (!s->i2c_rx_full && s->i2c_rx_pending) {
                /* fetch the lookahead byte */
                s->i2c_mrdr = i2c_recv(s->i2c_bus);
                s->i2c_rx_full = true;
                s->i2c_rx_pending--;
            }
            break;
        case LPI2C_CMD_STOP:
            i2c_end_transfer(s->i2c_bus);
            s->i2c_busy = false;
            s->i2c_rx_pending = 0;
            s->i2c_msr |= LPI2C_MSR_SDF | LPI2C_MSR_EPF;
            break;
        default:
            break;
        }
        mcxn_flexcomm_update_irq(s);
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled LPI2C write @0x%03" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, value);
        break;
    }
}

static uint64_t mcxn_lpuart_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);
    uint32_t r = 0;

    /* Wrapper registers are common to every function selection. */
    if (offset == LPFLEXCOMM_PSELID) {
        return s->pselid | PSELID_PRESENT_BITS | PSELID_ID_VALUE;
    }
    if (offset == LPFLEXCOMM_ISTAT) {
        return mcxn_flexcomm_istat(s);
    }
    /* Function-select: route to the LPSPI/LPI2C decode when chosen. */
    switch (s->pselid & PSELID_PERSEL) {
    case PERSEL_LPSPI:
        return mcxn_lpspi_read(s, offset);
    case PERSEL_LPI2C:
        if (offset < LPI2C_WINDOW) {
            return 0;    /* reserved below the LPI2C sub-block */
        }
        return mcxn_lpi2c_read(s, offset - LPI2C_WINDOW);
    default:
        break;  /* fall through to the LPUART register map */
    }

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
    case LPUART_MCR:
        r = s->mcr;
        break;
    case LPUART_MSR:
        /*
         * No modem lines are wired on this board: CTS/DSR/RI/DCD all
         * deasserted.
         */
        r = 0;
        break;
    case LPUART_STAT:
        /*
         * TX drains to the chardev instantly, so TXCOUNT is always 0 and
         * TDRE is always set -- an infinitely fast transmitter, which is the
         * honest emulation of a baud rate we do not model.
         *
         * RDRF is NOT "a byte arrived".  It is a LEVEL against the watermark,
         * and modelling it as "a byte arrived" is what let a 1-deep receiver
         * impersonate an 8-deep one for as long as nobody set RXWATER.
         */
        r = STAT_TDRE | STAT_TC;
        if (lpuart_rdrf(s)) {
            r |= STAT_RDRF;
        }
        r |= s->stat_or;          /* sticky overrun: a byte was DROPPED */
        break;
    case LPUART_CTRL:
        r = s->ctrl;
        break;
    case LPUART_DATA:
    case LPUART_DATARO:
        r = s->rx_count ? s->rx_fifo[s->rx_head] : 0;
        if (!s->rx_count) {
            /*
             * DATA[RXEMPT] (bit 12).  RM reset 0x0000_1000: an empty receiver
             * SAYS it is empty.  Reading 0 instead means "byte 0x00 was
             * received", and a guest polling DATA rather than STAT cannot tell
             * those apart -- a fabricated NUL in the input stream.
             */
            r |= LPUART_DATA_RXEMPT;
        }
        if (offset == LPUART_DATA && s->rx_count) {
            (void)lpuart_rx_pop(s);
            mcxn_flexcomm_update_irq(s);
            /*
             * The holding register is free again — tell the chardev to
             * resume delivering buffered input, or a continuous RX stream
             * stalls after one byte (can_rx returned 0 under flow control).
             */
            qemu_chr_fe_accept_input(&s->chr);
        }
        break;
    case LPUART_MATCH:
        r = s->match;
        break;
    case LPUART_MODIR:
        r = s->modir;
        break;
    case LPUART_FIFO:
        /*
         * FIFO is a CAPABILITY register: RXFIFOSIZE/TXFIFOSIZE are read-only
         * and describe the hardware.  RXEMPT/TXEMPT are LIVE and must follow
         * the FIFO -- a constant "empty" is a lie the moment there is anything
         * in it.
         */
        r = (s->fifo & ~LPUART_FIFO_SIZES_MASK) | LPUART_FIFO_SIZES;
        if (!s->rx_count) {
            r |= FIFO_RXEMPT;
        }
        r |= FIFO_TXEMPT;          /* TX drains instantly: always empty */
        break;
    case LPUART_WATER:
        /*
         * TXWATER/RXWATER are the guest's; TXCOUNT/RXCOUNT are OURS and
         * read-only.  A stored WATER that never reports a count is a register
         * that cannot answer the one question a FIFO driver asks it: HOW MANY
         * BYTES ARE THERE?
         */
        r = s->water & 0x00070007u;         /* TXWATER [2:0], RXWATER [18:16] */
        r |= ((uint32_t)s->rx_count & 0xF) << 24; /* RXCOUNT [27:24] */
                                                  /* TXCOUNT [11:8] stays 0 */
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

    /* Wrapper registers are common to every function selection. */
    if (offset == LPFLEXCOMM_PSELID) {
        s->pselid = value & (PSELID_PERSEL | PSELID_LOCK);
        return;
    }
    if (offset == LPFLEXCOMM_ISTAT) {
        return;  /* read-only */
    }
    /* Function-select: route to the LPSPI/LPI2C decode when chosen. */
    switch (s->pselid & PSELID_PERSEL) {
    case PERSEL_LPSPI:
        mcxn_lpspi_write(s, offset, value);
        return;
    case PERSEL_LPI2C:
        if (offset < LPI2C_WINDOW) {
            return;      /* reserved below the LPI2C sub-block */
        }
        mcxn_lpi2c_write(s, offset - LPI2C_WINDOW, value);
        return;
    default:
        break;  /* fall through to the LPUART register map */
    }

    switch (offset) {
    case LPUART_GLOBAL:
        s->global = value;
        if (value & GLOBAL_RST) {
            /* Software reset: clear the model's writable state. */
            s->ctrl = s->baud = s->fifo = s->water = 0;
            s->rx_head = s->rx_count = 0;
            s->stat_or = 0;
            mcxn_flexcomm_update_irq(s);
        }
        break;
    case LPUART_PINCFG:
        s->pincfg = value;
        break;
    case LPUART_MCR:
        s->mcr = value;
        break;
    case LPUART_MSR:
        break;                    /* status: W1C bits, none modelled */
    case LPUART_BAUD:
        /*
         * BAUD carries TDMAE/RDMAE — arming a DMA-enable bit changes
         * whether the transmitter/receiver is ASKING the eDMA for service,
         * so the request line must be re-evaluated here.  This used to be a
         * plain store, so a stock driver that armed TDMAE last (as they all
         * do) never raised a request at all.  ⚠ THE IDENTICAL BUG I HAD JUST
         * FIXED IN THE DAC's DER — a fix applied in one place is not a fix.
         */
        s->baud = value;
        mcxn_flexcomm_update_irq(s);
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
        mcxn_flexcomm_update_irq(s);
        break;
    case LPUART_DATA:
        ch = value & 0xFF;
        /* Honour TE: only transmit when the transmitter is enabled. */
        if (s->ctrl & CTRL_TE) {
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
        }
        mcxn_flexcomm_update_irq(s);
        break;
    case LPUART_MATCH:
        s->match = value;
        break;
    case LPUART_MODIR:
        s->modir = value;
        break;
    case LPUART_FIFO:
        /*
         * RXFIFOSIZE/TXFIFOSIZE are RO: the SDK writes this whole register
         * during init, and storing the written value would let software
         * overwrite the part's own description of its FIFO depth.
         */
        s->fifo = value & ~LPUART_FIFO_SIZES_MASK;
        /*
         * RXFLUSH/TXFLUSH are momentary strobes, not state.  A driver that
         * flushes a stale FIFO and then finds its bytes still there is being
         * lied to about the one operation whose entire purpose is to make the
         * buffer empty.
         */
        if (value & FIFO_RXFLUSH) {
            s->rx_count = 0;
            s->rx_head = 0;
            s->fifo &= ~FIFO_RXFLUSH;
            /* room again: pull queued input */
            qemu_chr_fe_accept_input(&s->chr);
        }
        s->fifo &= ~FIFO_TXFLUSH;                /* TX has nothing to flush */
        /* RXFE just changed the DEPTH, so RDRF may move */
        mcxn_flexcomm_update_irq(s);
        break;
    case LPUART_WATER:
        /* Only the watermarks are writable.  The counts are the hardware's. */
        s->water = value & 0x00070007u;
        /* RDRF is a LEVEL vs RXWATER: it may move NOW */
        mcxn_flexcomm_update_irq(s);
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
    case LPUART_VERID:
    case LPUART_PARAM:
    case LPUART_DATARO:
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
    /*
     * Accept 1/2/4-byte access.  A real driver moving data over eDMA bursts the
     * FIFO data registers (LPUART DATA, LPSPI TDR/RDR) one byte at a time; an
     * over-strict 4-byte-only window would make the memory core SILENTLY DROP
     * those byte writes (transfer "completes" but no data reaches the
     * wire) — a silent-wrong-answer bug (fleet lesson from the i.MX95
     * LPSPI-over-eDMA
     * case).  impl.min=1 routes each byte straight to the handler (no
     * read-modify-write, so the DATA-read RX side effect is not spuriously
     * triggered by a byte write); the data registers hold their value in the
     * low byte, so aligned byte access is handled correctly.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static int mcxn_lpuart_can_rx(void *opaque)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);

    /*
     * Offer the REAL free space.  This used to return a single slot, so
     * QEMU handed us one byte at a time and an 8-deep FIFO could never
     * actually hold 8 bytes -- the flow control silently enforced the
     * depth-1 model the FIFO register denied.
     */
    if (!(s->ctrl & CTRL_RE)) {
        return 0;
    }
    return lpuart_rx_depth(s) - s->rx_count;
}

static void mcxn_lpuart_rx(void *opaque, const uint8_t *buf, int size)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);
    int i;

    for (i = 0; i < size; i++) {
        if (!lpuart_rx_push(s, buf[i])) {
            /*
             * ⭐ FAIL TO THE GUEST, NOT JUST THE LOG.  The byte is GONE.
             *   STAT[OR] is the block's own documented, non-gating channel for
             *   exactly this, and a receiver that drops data silently is
             *   indistinguishable from a sender that never sent it.
             */
            /*
             * ⚠ STATED GAP: A SOCKET-BACKED LPUART CANNOT REACH THIS.
             *
             * can_receive() reports our free space, so QEMU BACKPRESSURES the
             * sender and HOLDS the byte rather than handing it to us.  Real
             * silicon has no such flow control -- a byte on the wire arrives
             * whether or not there is room, and STAT[OR] fires.  So this path
             * is correct and, with a chardev backend, UNREACHABLE: we never
             * lose data, and the guest never sees an overrun it WOULD see on
             * the board.
             *
             * That is a divergence in the FORGIVING direction, and it is the
             * chardev abstraction's, not ours -- but it is ours to STATE.  A
             * backend that ignores can_receive (or a future in-model
             * line-rate) reaches this, and when it does, the guest learns the
             * truth through the block's own documented, non-gating channel
             * rather than losing bytes in silence.
             */
            s->stat_or |= STAT_OR;
            qemu_log_mask(LOG_GUEST_ERROR,
                          "mcxn_lpuart: RX overrun -- byte 0x%02x dropped "
                          "(depth %u, RXFE=%d)\n",
                          buf[i], lpuart_rx_depth(s), !!(s->fifo & FIFO_RXFE));
        }
    }
    mcxn_flexcomm_update_irq(s);
}

static void mcxn_lpuart_reset(DeviceState *dev)
{
    MCXNLPUARTState *s = MCXN_LPUART(dev);

    s->global = s->pincfg = s->ctrl = 0;
    s->match = s->modir = s->fifo = s->water = 0;
    /*
     * PERSEL resets to 0 = NO FUNCTION SELECTED (RM).  The decode falls
     * through to the LPUART register map on PERSEL 0, so the console
     * still works -- but the register now REPORTS what silicon reports,
     * instead of claiming a selection the guest never made.
     */
    s->pselid = 0;
    s->reir = s->teir = s->hdcr = s->tocr = 0;
    /* Reset values from the RM, not zero -- see LPUART_BAUD_RESET above. */
    s->baud = LPUART_BAUD_RESET;
    s->tosr = LPUART_TOSR_RESET;
    s->timeout[0] = s->timeout[1] = s->timeout[2] = s->timeout[3] = 0;
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    s->rx_head = s->rx_count = 0;
    s->stat_or = 0;

    /* LPSPI / LPI2C function state. */
    s->spi_cr = s->spi_sr = s->spi_ier = 0;
    s->spi_cfgr0 = s->spi_cfgr1 = s->spi_ccr = s->spi_fcr = 0;
    s->spi_tcr = LPSPI_TCR_RESET;
    s->spi_rdr = 0;
    s->spi_rx_full = false;
    s->spi_cs_asserted = false;
    s->i2c_mcr = s->i2c_msr = s->i2c_mier = s->i2c_mcfgr1 = 0;
    s->i2c_mrdr = 0;
    s->i2c_rx_full = s->i2c_busy = false;
    s->i2c_rx_pending = 0;
}

static void mcxn_lpuart_realize(DeviceState *dev, Error **errp)
{
    MCXNLPUARTState *s = MCXN_LPUART(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_lpuart_ops, s,
                          TYPE_MCXN_LPUART, 0x1000);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->dma_req_tx);   /* -> eDMA source 70 + 2n */
    sysbus_init_irq(sbd, &s->dma_req_rx);   /* -> eDMA source 69 + 2n */
    /* 3: LPSPI chip-select (to an SSI device) */
    sysbus_init_irq(sbd, &s->spi_cs);

    qemu_chr_fe_set_handlers(&s->chr, mcxn_lpuart_can_rx, mcxn_lpuart_rx,
                             NULL, NULL, s, NULL, true);

    /*
     * Board-to-board LPSPI node: expose a named SSI bus so a `spi-link`
     * peripheral can bridge this FlexComm's LPSPI to a chardev socket.
     */
    if (s->spi_bus_name) {
        s->spi_bus = ssi_create_bus(dev, s->spi_bus_name);
    }

    /*
     * The FlexComm's LPI2C function drives a REAL I2C bus, named
     * "<flexcommN>-i2c" so a test can attach a genuine device to a
     * specific FlexComm:
     *   -device at24c-eeprom,bus=flexcomm0-i2c,address=0x50
     */
    {
        g_autofree char *busname = g_strdup_printf(
            "%s-i2c", object_get_canonical_path_component(OBJECT(dev)));
        s->i2c_bus = i2c_init_bus(dev, busname);
    }
}

static const VMStateDescription vmstate_mcxn_lpuart = {
    .name = TYPE_MCXN_LPUART,
    .version_id = 4,
    .minimum_version_id = 4,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(global, MCXNLPUARTState),
        VMSTATE_UINT32(pincfg, MCXNLPUARTState),
        VMSTATE_UINT32(baud, MCXNLPUARTState),
        VMSTATE_UINT32(ctrl, MCXNLPUARTState),
        VMSTATE_UINT32(match, MCXNLPUARTState),
        VMSTATE_UINT32(modir, MCXNLPUARTState),
        VMSTATE_UINT32(fifo, MCXNLPUARTState),
        VMSTATE_UINT32(water, MCXNLPUARTState),
        /*
         * The RX FIFO MUST migrate.  91emulator's sdhci `vendor_spec` was
         * "not in the vmstate at all", so a snapshot came back with the
         * hardware desynced from the register controlling it.  Buffered bytes
         * are state; state that is not migrated is state that silently
         * vanishes across a snapshot.
         */
        VMSTATE_UINT8_ARRAY(rx_fifo, MCXNLPUARTState, MCXN_LPUART_FIFO_DEPTH),
        VMSTATE_UINT8(rx_head, MCXNLPUARTState),
        VMSTATE_UINT8(rx_count, MCXNLPUARTState),
        VMSTATE_UINT32(stat_or, MCXNLPUARTState),
        VMSTATE_UINT32(pselid, MCXNLPUARTState),
        VMSTATE_UINT32(reir, MCXNLPUARTState),
        VMSTATE_UINT32(teir, MCXNLPUARTState),
        VMSTATE_UINT32(hdcr, MCXNLPUARTState),
        VMSTATE_UINT32(tocr, MCXNLPUARTState),
        VMSTATE_UINT32(tosr, MCXNLPUARTState),
        VMSTATE_UINT32_ARRAY(timeout, MCXNLPUARTState, 4),
        /* LPSPI function */
        VMSTATE_UINT32(spi_cr, MCXNLPUARTState),
        VMSTATE_UINT32(spi_sr, MCXNLPUARTState),
        VMSTATE_UINT32(spi_ier, MCXNLPUARTState),
        VMSTATE_UINT32(spi_cfgr0, MCXNLPUARTState),
        VMSTATE_UINT32(spi_cfgr1, MCXNLPUARTState),
        VMSTATE_UINT32(spi_ccr, MCXNLPUARTState),
        VMSTATE_UINT32(spi_fcr, MCXNLPUARTState),
        VMSTATE_UINT32(spi_tcr, MCXNLPUARTState),
        VMSTATE_UINT32(spi_rdr, MCXNLPUARTState),
        VMSTATE_BOOL(spi_rx_full, MCXNLPUARTState),
        /* LPI2C function */
        VMSTATE_UINT32(i2c_mcr, MCXNLPUARTState),
        VMSTATE_UINT32(i2c_msr, MCXNLPUARTState),
        VMSTATE_UINT32(i2c_mier, MCXNLPUARTState),
        VMSTATE_UINT32(i2c_mcfgr1, MCXNLPUARTState),
        VMSTATE_UINT32(i2c_mrdr, MCXNLPUARTState),
        VMSTATE_BOOL(i2c_rx_full, MCXNLPUARTState),
        VMSTATE_BOOL(i2c_busy, MCXNLPUARTState),
        VMSTATE_UINT32(i2c_rx_pending, MCXNLPUARTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mcxn_lpuart_properties[] = {
    DEFINE_PROP_CHR("chardev", MCXNLPUARTState, chr),
    DEFINE_PROP_STRING("spi-bus-name", MCXNLPUARTState, spi_bus_name),
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
