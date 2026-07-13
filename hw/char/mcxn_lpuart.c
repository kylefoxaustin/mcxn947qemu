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
#include "migration/vmstate.h"

/* --- LPUART core register offsets ------------------------------------------ */
#define LPUART_VERID    0x00  /* RO */
#define LPUART_PARAM    0x04  /* RO */
#define LPUART_GLOBAL   0x08
#define LPUART_PINCFG   0x0C
#define LPUART_MCR      0x40   /* Modem Control -- the STOCK EDMA driver reads
                                * AND writes this; it was unhandled and showed up
                                * only when a REAL driver was run against the
                                * model, never in firmware I wrote myself. */
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

/* --- LP_FLEXCOMM wrapper register offsets ---------------------------------- */
#define LPFLEXCOMM_ISTAT   0xFF4  /* RO */
#define LPFLEXCOMM_PSELID  0xFF8

/*
 * ⚠ LPI2C LIVES AT +0x800 INSIDE THE FLEXCOMM WINDOW.  LPUART AND LPSPI DO NOT.
 *
 * CMSIS is unambiguous, and the three do not share a base:
 *     LP_FLEXCOMM0_BASE = 0x4009_2000
 *     LPUART0_BASE      = 0x4009_2000    (+0x000)
 *     LPSPI0_BASE       = 0x4009_2000    (+0x000)   -- overlays LPUART
 *     LPI2C0_BASE       = 0x4009_2800    (+0x800)   -- DOES NOT
 *
 * This decode had LPI2C at +0x000 alongside the other two.  So every LPI2C
 * register access from real firmware -- which naturally uses LPI2C0_BASE -- landed
 * at window offset 0x8xx, MATCHED NO CASE, AND WAS SILENTLY DROPPED: reads returned
 * 0, writes went nowhere.  THE WHOLE IP WAS UNREACHABLE FROM THE GUEST.  The stock
 * lpi2c examples print their banner and then quietly do nothing, forever, because
 * LPI2C_MasterInit()'s every write fell on the floor.
 *
 * And my own LPI2C tests passed the entire time, because they poked THE OFFSETS
 * THIS FILE INVENTED.  The test agreed with the model because the test got its
 * address from the model.  That is not a test, it is a mirror -- and no amount of
 * mutation testing can see it, because mutating the model moves the mirror too.
 * It took an INDEPENDENT golden (the RM's reset values, read back at the addresses
 * CMSIS gives) to notice that nobody was home.
 */
#define LPI2C_WINDOW  0x800

/*
 * Reset values from the RM's register map.  Each of these was ZERO, and zero is a
 * CLAIM the guest acts on -- see the SCG SIRCCSR bug that started this audit.
 */
#define LPUART_BAUD_RESET   0x0F000004u  /* OSR=15, SBR=4 -- the SDK DIVIDES by OSR */
#define LPUART_TOSR_RESET   0x0000000Fu
#define LPSPI_TCR_RESET     0x0000001Fu  /* FRAMESZ = 31 -> a 32-bit frame */

/*
 * FIFO[RXFIFOSIZE] (bits 2:0) and FIFO[TXFIFOSIZE] (bits 6:4) are READ-ONLY
 * CAPABILITY fields: the part telling software how deep its FIFOs are.  Reset 0x22
 * = size code 2 on each.  They were 0 (= the smallest FIFO), and worse, they were
 * STORED -- so the SDK's `base->FIFO = ...` during init would have OVERWRITTEN the
 * part's own description of itself.  A capability register that software can change
 * is not a capability register.
 */
#define LPUART_FIFO_SIZES      0x00000022u
#define LPUART_FIFO_SIZES_MASK 0x00000077u
#define LPUART_DATA_RXEMPT     0x00001000u  /* CMSIS LPUART_DATA_RXEMPT_MASK */

/* --- Bit masks (CMSIS) ----------------------------------------------------- */
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
 * on UARTPRESENT via LP_FLEXCOMM_PeripheralIsPresent() — without it the console
 * driver bails before programming BAUD/CTRL and PRINTF silently emits nothing.
 */
#define PSELID_UARTPRESENT  0x10u
#define PSELID_SPIPRESENT   0x20u
#define PSELID_I2CPRESENT   0x40u
#define PSELID_PRESENT_BITS \
    (PSELID_UARTPRESENT | PSELID_SPIPRESENT | PSELID_I2CPRESENT)

/*
 * PSELID[ID] (bits 31:12) -- the block identifying itself.  RM reset for the whole
 * register is 0x0010_3070: the three PRESENT bits (0x70), the ID field (0x103000),
 * and PERSEL = 0 (NO FUNCTION SELECTED).
 *
 * We returned 0x71: the present bits, NO ID AT ALL, and PERSEL = 1 (LPUART already
 * chosen).  Both halves were wrong -- the part under-reported what it is, and it
 * claimed a function selection the guest had not made.
 */
#define PSELID_ID_VALUE  0x00103000u

/*
 * VERID/PARAM are read by some HALs to size the FIFO.  Values are plausible
 * MCX-class constants; refine against the RM if a HAL ever depends on them.
 */
#define LPUART_VERID_VALUE  0x04010003u
#define LPUART_PARAM_VALUE  0x00000404u  /* TX/RX FIFO depth fields */

/* PERSEL function selections (LP_FLEXCOMM_PERIPH_T, CMSIS enum). */
#define PERSEL_LPSPI    2u
#define PERSEL_LPI2C    3u

/* LP_FLEXCOMM ISTAT (0xFF4) per-function interrupt-pending bits (CMSIS). */
#define ISTAT_UARTTX    0x1u
#define ISTAT_UARTRX    0x2u
#define ISTAT_SPI       0x4u
#define ISTAT_I2CM      0x10u

/* === LPSPI register offsets (PERSEL = 2, master view) ====================== */
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
#define LPSPI_RSR_RXEMPTY 0x2u

#define LPSPI_VERID_VALUE  0x01010004u
#define LPSPI_PARAM_VALUE  0x00040404u  /* PCSNUM=4, RX/TX FIFO depth exp=4 */

/* === LPI2C register offsets (PERSEL = 3, controller/master view) =========== */
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
#define LPI2C_CMD_START    4u   /* generate (re)START + transmit address; 4..7 are START variants */

#define LPI2C_VERID_VALUE  0x01000003u
#define LPI2C_PARAM_VALUE  0x00000202u  /* M TX/RX FIFO depth exp=2 (4 deep) */

/* Dynamic LPSPI status: latched W1C flags plus the always-current TDF/RDF. */
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
     * TDF means "the TX FIFO is at or below its watermark", i.e. THERE IS ROOM.
     * That is true of an empty FIFO whether or not the module is enabled, which is
     * why the RM gives MSR a reset value of 1 with MEN still clear.  Gating it on
     * MEN made TDF mean "enabled AND has room" -- a different claim, and one that
     * reads back 0 out of reset where silicon reads 1.
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
        /* TX data-register-empty and transmit-complete are always asserted in
         * this model (writes are synchronous), so TIE/TCIE assert immediately. */
        if (s->ctrl & (CTRL_TIE | CTRL_TCIE)) {
            istat |= ISTAT_UARTTX;
        }
        if ((s->ctrl & CTRL_RIE) && s->rx_full) {
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
 * capability is still a missing capability.  Naming a gap in the place you first
 * met it is not the same as understanding its extent.  The flag discharges the
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
        /* TDRE is always asserted here (writes are synchronous), so an armed
         * TX DMA request is continuously asserted until the channel's major
         * loop completes and TCD_CSR[DREQ] clears ERQ — which is exactly how a
         * real UART TX DMA drains a buffer. */
        tx = (s->baud & BAUD_TDMAE) != 0;
        rx = (s->baud & BAUD_RDMAE) && s->rx_full;
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
    case LPSPI_FSR:   return s->spi_rx_full ? (1u << 16) : 0; /* RXCOUNT=1 */
    case LPSPI_RSR:   return s->spi_rx_full ? 0 : LPSPI_RSR_RXEMPTY;
    case LPSPI_RDROR: return s->spi_rdr;                       /* peek, no pop */
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
    case LPSPI_CFGR0: s->spi_cfgr0 = value; break;
    case LPSPI_CFGR1: s->spi_cfgr1 = value; break;
    case LPSPI_CCR:   s->spi_ccr = value;   break;
    case LPSPI_FCR:   s->spi_fcr = value;   break;
    case LPSPI_TCR:   s->spi_tcr = value;   break;
    case LPSPI_CCR1:
        break;  /* accepted, not modelled */
    case LPSPI_DER:
        /* The DMA-enable bits.  These used to be ACCEPTED AND DISCARDED, so the
         * stock LPSPI_MasterTransferEDMA driver armed a channel, set TDDE, and
         * waited forever for a request nothing could raise. */
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
                uint32_t framesz = (s->spi_tcr & LPSPI_TCR_FRAMESZ) + 1; /* bits */
                uint32_t mask = (framesz >= 32) ? 0xFFFFFFFFu
                                                : ((1u << framesz) - 1);
                if (s->spi_bus) {
                    /* Board-to-board: shift the word out over the SSI bus (to a
                     * spi-link -> socket -> peer); MISO comes back from the bus. */
                    s->spi_rdr = ssi_transfer(s->spi_bus, value & mask) & mask;
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
    case LPI2C_MFSR:   return s->i2c_rx_full ? (1u << 16) : 0;  /* RXCOUNT=1 */
    /*
     * ⚠ MRDR WAS RIGHT AND ITS ALIAS WAS WRONG, WHICH IS THE WHOLE POINT.
     *
     * MRDROR is the NON-DESTRUCTIVE alias of MRDR -- a peek that does not pop.  It was
     * not modelled at all, so it fell through to the default and RETURNED ZERO.  And
     * zero is not "nothing": RXEMPTY is bit 14, so zero means THE RECEIVE FIFO HAS DATA.
     *
     *     ⭐ AN UNMODELLED REGISTER IS NOT A FREE REGISTER.  IT STILL ANSWERS -- AND
     *        ZERO IS AN ANSWER.  (91emulator, who shipped the identical bug: their MRDR
     *        was correct and its alias was not.)
     *
     * A driver that polls the non-destructive alias -- exactly what an alias is FOR --
     * saw RXEMPTY clear and read a PHANTOM BYTE out of an empty FIFO.
     *
     * The slave registers (SASR/SRDR/SRDROR) are likewise unmodelled, and likewise were
     * answering "data available" to anyone who asked.  We do not model the LPI2C slave
     * engine, so they now report HONESTLY EMPTY.  A missing feature that says "empty" is
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
            s->i2c_rx_full = false;
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
    case LPI2C_MCFGR1: s->i2c_mcfgr1 = value; break;
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
         * Controller command FIFO.  No external I2C bus is modelled; instead a
         * tiny echo target ACKs every address and returns the most recently
         * transmitted byte on a receive command, so a master read-after-write
         * sequence completes deterministically (the FlexCAN/I3C loopback idiom).
         */
        uint32_t cmd = (value & LPI2C_MTDR_CMD_MASK) >> LPI2C_MTDR_CMD_SHIFT;
        uint8_t data = value & LPI2C_MTDR_DATA_MASK;

        if (!(s->i2c_mcr & LPI2C_MCR_MEN)) {
            break;
        }
        switch (cmd) {
        case LPI2C_CMD_START:
        case 5: case 6: case 7:          /* all START + address variants */
            s->i2c_busy = true;          /* target present -> ACK, no NDF */
            break;
        case LPI2C_CMD_TXDATA:
            s->i2c_last_tx = data;       /* echoed back by a receive command */
            break;
        case LPI2C_CMD_RXDATA:
            s->i2c_mrdr = s->i2c_last_tx;
            s->i2c_rx_full = true;
            break;
        case LPI2C_CMD_STOP:
            s->i2c_busy = false;
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
        /* No modem lines are wired on this board: CTS/DSR/RI/DCD all deasserted. */
        r = 0;
        break;
    case LPUART_STAT:
        /* TX always ready; RDRF reflects the 1-byte rx holding register. */
        r = STAT_TDRE | STAT_TC;
        if (s->rx_full) {
            r |= STAT_RDRF;
        }
        break;
    case LPUART_CTRL:
        r = s->ctrl;
        break;
    case LPUART_DATA:
    case LPUART_DATARO:
        r = s->rx_byte;
        if (!s->rx_full) {
            /* DATA[RXEMPT] (bit 12).  RM reset 0x0000_1000: an empty receiver SAYS
             * it is empty.  Reading 0 instead means "byte 0x00 was received", and a
             * guest polling DATA rather than STAT cannot tell those apart -- a
             * fabricated NUL in the input stream. */
            r |= LPUART_DATA_RXEMPT;
        }
        if (offset == LPUART_DATA && s->rx_full) {
            s->rx_full = false;
            mcxn_flexcomm_update_irq(s);
            /* The holding register is free again — tell the chardev to resume
             * delivering buffered input, or a continuous RX stream stalls after
             * one byte (can_rx returned 0 under flow control). */
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
        r = (s->fifo & ~LPUART_FIFO_SIZES_MASK) | LPUART_FIFO_SIZES | FIFO_TXEMPT;
        if (!s->rx_full) {
            r |= FIFO_RXEMPT;
        }
        break;
    case LPUART_WATER:
        r = s->water;
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
            s->rx_full = false;
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
        /* BAUD carries TDMAE/RDMAE — arming a DMA-enable bit changes whether the
         * transmitter/receiver is ASKING the eDMA for service, so the request
         * line must be re-evaluated here.  This used to be a plain store, so a
         * stock driver that armed TDMAE last (as they all do) never raised a
         * request at all.  ⚠ THE IDENTICAL BUG I HAD JUST FIXED IN THE DAC's DER
         * — a fix applied in one place is not a fix. */
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
        /* RXFIFOSIZE/TXFIFOSIZE are RO: the SDK writes this whole register during
         * init, and storing the written value would let software overwrite the
         * part's own description of its FIFO depth. */
        s->fifo = value & ~LPUART_FIFO_SIZES_MASK;
        break;
    case LPUART_WATER:
        s->water = value;
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
     * those byte writes (transfer "completes" but no data reaches the wire) — a
     * silent-wrong-answer bug (fleet lesson from the i.MX95 LPSPI-over-eDMA
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
    /* Accept a byte only when the receiver is enabled and the holding reg is
     * empty (single-entry model). */
    return (s->ctrl & CTRL_RE) && !s->rx_full;
}

static void mcxn_lpuart_rx(void *opaque, const uint8_t *buf, int size)
{
    MCXNLPUARTState *s = MCXN_LPUART(opaque);

    if (size > 0) {
        s->rx_byte = buf[0];
        s->rx_full = true;
        mcxn_flexcomm_update_irq(s);
    }
}

static void mcxn_lpuart_reset(DeviceState *dev)
{
    MCXNLPUARTState *s = MCXN_LPUART(dev);

    s->global = s->pincfg = s->ctrl = 0;
    s->match = s->modir = s->fifo = s->water = 0;
    /*
     * PERSEL resets to 0 = NO FUNCTION SELECTED (RM).  The decode falls through to
     * the LPUART register map on PERSEL 0, so the console still works -- but the
     * register now REPORTS what silicon reports, instead of claiming a selection the
     * guest never made.
     */
    s->pselid = 0;
    s->reir = s->teir = s->hdcr = s->tocr = 0;
    /* Reset values from the RM, not zero -- see LPUART_BAUD_RESET above. */
    s->baud = LPUART_BAUD_RESET;
    s->tosr = LPUART_TOSR_RESET;
    s->timeout[0] = s->timeout[1] = s->timeout[2] = s->timeout[3] = 0;
    s->rx_byte = 0;
    s->rx_full = false;

    /* LPSPI / LPI2C function state. */
    s->spi_cr = s->spi_sr = s->spi_ier = 0;
    s->spi_cfgr0 = s->spi_cfgr1 = s->spi_ccr = s->spi_fcr = 0;
    s->spi_tcr = LPSPI_TCR_RESET;
    s->spi_rdr = 0;
    s->spi_rx_full = false;
    s->i2c_mcr = s->i2c_msr = s->i2c_mier = s->i2c_mcfgr1 = 0;
    s->i2c_mrdr = 0;
    s->i2c_rx_full = s->i2c_busy = false;
    s->i2c_last_tx = 0;
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

    qemu_chr_fe_set_handlers(&s->chr, mcxn_lpuart_can_rx, mcxn_lpuart_rx,
                             NULL, NULL, s, NULL, true);

    /* Board-to-board LPSPI node: expose a named SSI bus so a `spi-link`
     * peripheral can bridge this FlexComm's LPSPI to a chardev socket. */
    if (s->spi_bus_name) {
        s->spi_bus = ssi_create_bus(dev, s->spi_bus_name);
    }
}

static const VMStateDescription vmstate_mcxn_lpuart = {
    .name = TYPE_MCXN_LPUART,
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(global, MCXNLPUARTState),
        VMSTATE_UINT32(pincfg, MCXNLPUARTState),
        VMSTATE_UINT32(baud, MCXNLPUARTState),
        VMSTATE_UINT32(ctrl, MCXNLPUARTState),
        VMSTATE_UINT32(match, MCXNLPUARTState),
        VMSTATE_UINT32(modir, MCXNLPUARTState),
        VMSTATE_UINT32(fifo, MCXNLPUARTState),
        VMSTATE_UINT32(water, MCXNLPUARTState),
        VMSTATE_UINT32(pselid, MCXNLPUARTState),
        VMSTATE_UINT32(reir, MCXNLPUARTState),
        VMSTATE_UINT32(teir, MCXNLPUARTState),
        VMSTATE_UINT32(hdcr, MCXNLPUARTState),
        VMSTATE_UINT32(tocr, MCXNLPUARTState),
        VMSTATE_UINT32(tosr, MCXNLPUARTState),
        VMSTATE_UINT32_ARRAY(timeout, MCXNLPUARTState, 4),
        VMSTATE_UINT8(rx_byte, MCXNLPUARTState),
        VMSTATE_BOOL(rx_full, MCXNLPUARTState),
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
        VMSTATE_UINT8(i2c_last_tx, MCXNLPUARTState),
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
