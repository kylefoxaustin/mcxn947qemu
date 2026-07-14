/*
 * NXP MCX N LP_FLEXCOMM — LPUART / LPSPI / LPI2C function-selectable model
 *
 * Models one FlexComm instance.  The selected function's core registers sit at
 * the bottom of the 4 KiB block; the LP_FLEXCOMM wrapper (ISTAT @ 0xFF4,
 * PSELID @ 0xFF8) sits at the top.  PSELID.PERSEL selects LPUART (1), LPSPI (2)
 * or LPI2C (3); the device decodes the matching register map and they share one
 * NVIC line.  The LPUART layout is the standard NXP LPUART, identical to
 * i.MX 93/95.  LPSPI/LPI2C model the master/controller transfer engine with
 * internal loopback/echo (see mcxn_lpuart.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_MCXN_LPUART_H
#define HW_CHAR_MCXN_LPUART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_MCXN_LPUART "mcxn-lpuart"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNLPUARTState, MCXN_LPUART)

/* RM: FIFO[RXFIFOSIZE] resets to 010b, and the RM's table reads "010b - 8". */
#define MCXN_LPUART_FIFO_DEPTH 8

struct MCXNLPUARTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    CharFrontend  chr;

    /* DMA request lines into the eDMA: LpFlexcomm{n} Rx = 69 + 2n, Tx = 70 + 2n
     * (CMSIS).  Without these, every stock *_TransferSendEDMA driver hangs. */
    qemu_irq     dma_req_tx;
    qemu_irq     dma_req_rx;
    bool         dma_tx_level;
    bool         dma_rx_level;

    /* When this FlexComm is used as an LPSPI board-to-board node, spi_bus_name
     * is set and an SSI bus is created so a `spi-link` peripheral can bridge it
     * to a chardev socket; LPSPI-mode TDR writes then shift over that bus
     * instead of the internal loopback.  NULL for console/loopback instances. */
    char    *spi_bus_name;
    SSIBus  *spi_bus;

    /* Register state (only what the console path needs is meaningful). */
    uint32_t global;
    uint32_t pincfg;
    uint32_t baud;
    uint32_t ctrl;
    uint32_t match;
    uint32_t modir;
    uint32_t fifo;
    uint32_t water;
    uint32_t pselid;     /* LP_FLEXCOMM peripheral-select */
    uint32_t mcr;        /* Modem Control @0x40 (the stock EDMA driver uses it) */

    /* Extended/timeout registers the SDK driver zeroes during init (0x48..0x6C). */
    uint32_t reir;       /* Receiver Extended Idle    @0x48 */
    uint32_t teir;       /* Transmitter Extended Idle @0x4C */
    uint32_t hdcr;       /* Half Duplex Control       @0x50 */
    uint32_t tocr;       /* Timeout Control           @0x58 */
    uint32_t tosr;       /* Timeout Status            @0x5C */
    uint32_t timeout[4]; /* Timeout 0..3              @0x60..0x6C */

    /*
     * ⚠ THE RX PATH USED TO BE A SINGLE BYTE (`rx_byte` + `rx_full`) WHILE THE FIFO
     *   REGISTER ADVERTISED AN 8-DEEP RECEIVE FIFO.  The advertisement was CORRECT --
     *   RM FIFO reset = 0x00C0_0022, RXFIFOSIZE = 010b, and the RM's own table reads
     *   "010b - 8" -- so the model told the truth about the CHIP and a lie about
     *   ITSELF.
     *
     *   ⭐ A CAPABILITY REGISTER IS A CONTRACT.  We promised eight and delivered one.
     *
     *   And it was INVISIBLE, because RDRF means "RXCOUNT > RXWATER" (RM, verbatim)
     *   and RXWATER resets to 0 -- so with no watermark set, depth-1 and depth-8
     *   behave identically.  Nothing in 70 suites ever set a watermark.  A driver
     *   that did (enable RXFE, RXWATER=3, wait for RDRF, read 4) was woken on the
     *   FIRST byte and read three stale ones: SILENT DATA CORRUPTION on the console
     *   UART, the most-exercised block in the tree.
     */
    uint8_t  rx_fifo[MCXN_LPUART_FIFO_DEPTH];
    uint8_t  rx_head;
    uint8_t  rx_count;
    uint32_t stat_or;    /* STAT[OR]: sticky overrun -- a byte arrived with no room */

    /*
     * LP_FLEXCOMM SPI / I2C function state.  The same 4 KiB window decodes as
     * LPSPI (PERSEL=2) or LPI2C (PERSEL=3) instead of LPUART.  These model the
     * controller (master) transfer engine only — there is no external SPI/I2C
     * bus, so a master transfer completes synchronously with internal
     * loopback/echo, mirroring the FlexCAN MB-loopback and I3C completion
     * models.  All offsets/bits are from the MCXN947 CMSIS header.
     */
    /* LPSPI (master) */
    uint32_t spi_cr;
    uint32_t spi_sr;      /* latched W1C flags (WCF/FCF/TCF) */
    uint32_t spi_ier;
    uint32_t spi_cfgr0;
    uint32_t spi_cfgr1;
    uint32_t spi_ccr;
    uint32_t spi_fcr;
    uint32_t spi_tcr;
    uint32_t spi_der;     /* DMA Enable — the stock EDMA driver sets TDDE/RDDE */
    uint32_t spi_rdr;     /* rx data holding */
    bool     spi_rx_full;

    /* LPI2C (master) */
    uint32_t i2c_mcr;
    uint32_t i2c_msr;     /* latched W1C flags (EPF/SDF/NDF) */
    uint32_t i2c_mier;
    uint32_t i2c_mcfgr1;
    uint32_t i2c_mder;    /* DMA Enable — the stock EDMA driver sets TDDE/RDDE */
    uint32_t i2c_mrdr;    /* rx data holding */
    bool     i2c_rx_full;
    bool     i2c_busy;    /* asserted between START and STOP */
    uint8_t  i2c_last_tx; /* echoed back by a receive command */
};

#endif /* HW_CHAR_MCXN_LPUART_H */
