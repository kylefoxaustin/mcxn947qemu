/*
 * MCXN947 LPUART TX driven entirely by the eDMA — the shape every stock UART
 * driver uses (LPUART_TransferSendEDMA), and which could not run at all.
 *
 * THE GAP.  eDMA request lines existed for SAI and DAC only.  The FlexComm block
 * — all ten LPUART/LPSPI/LPI2C instances — drove NOTHING, and LPSPI's `DER`
 * register (the DMA-enable the driver sets) was literally "accepted, not
 * modelled": the driver wrote its enable bit and the model THREW IT AWAY.  So a
 * guest arming a channel at the UART data register, setting BAUD[TDMAE], and
 * waiting for the FIFO to ask for data waited FOREVER.
 *
 * I HAD ALREADY FLAGGED THIS GAP IN THE DOCS AND STOPPED THERE.  rt1180emulator
 * named the organ:
 *
 *     "An honestly-documented missing capability is still a missing capability.
 *      Naming a gap in the place you first met it is not the same as
 *      understanding its extent.  THE FLAG DISCHARGES THE ANXIETY AND THE GAP
 *      STAYS."
 *
 * He flagged his in one row and it was a gap in twelve.  Mine said "SAI and DAC
 * only" — and the chip has 117 request sources.
 *
 * THE CPU NEVER WRITES LPUART_DATA.  Every byte on the wire is carried there by
 * the eDMA because the transmitter asked for it.  If the request line does not
 * work, NOTHING is printed and the harness fails on absent output — it cannot
 * pass by accident.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4  0x400B4000u
#define LP_BAUD  (*(volatile uint32_t *)(LPUART4 + 0x10))
#define LP_STAT  (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL  (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA_ADDR             (LPUART4 + 0x1C)
#define LP_DATA  (*(volatile uint32_t *)(LP_DATA_ADDR))

#define CTRL_TE    (1u << 19)
#define STAT_TDRE  (1u << 23)
#define BAUD_TDMAE (1u << 23)      /* LPUART_BAUD[TDMAE] */

/* ---- DMA0 ---------------------------------------------------------------- */
#define DMA0 0x40080000u
#define CH(n) (DMA0 + 0x1000u * ((n) + 1))
#define CH_CSR(n)    (*(volatile uint32_t *)(CH(n) + 0x00))
#define CH_MUX(n)    (*(volatile uint32_t *)(CH(n) + 0x14))
#define TCD_SADDR(n) (*(volatile uint32_t *)(CH(n) + 0x20))
#define TCD_SOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x24))
#define TCD_ATTR(n)  (*(volatile uint16_t *)(CH(n) + 0x26))
#define TCD_NBYTES(n)(*(volatile uint32_t *)(CH(n) + 0x28))
#define TCD_SLAST(n) (*(volatile uint32_t *)(CH(n) + 0x2C))
#define TCD_DADDR(n) (*(volatile uint32_t *)(CH(n) + 0x30))
#define TCD_DOFF(n)  (*(volatile uint16_t *)(CH(n) + 0x34))
#define TCD_CITER(n) (*(volatile uint16_t *)(CH(n) + 0x36))
#define TCD_DLAST(n) (*(volatile uint32_t *)(CH(n) + 0x38))
#define TCD_CSR(n)   (*(volatile uint16_t *)(CH(n) + 0x3C))
#define TCD_BITER(n) (*(volatile uint16_t *)(CH(n) + 0x3E))

#define CSR_ERQ    (1u << 0)
#define CSR_DONE   (1u << 30)
#define TCD_DREQ   (1u << 3)
#define ATTR_8BIT  0u              /* SSIZE = DSIZE = 0 -> 1 byte */

/* FlexComm4 = LpFlexcomm4 Tx = 70 + 2*4 = 78 (CMSIS dma_request_source_t). */
#define DMAREQ_FC4_TX 78
#define CHAN 2

#define MSGLEN 26
static volatile uint8_t msg[MSGLEN];

void cpu0_main(void)
{
    volatile int d;
    int i;

    /* "UARTDMA PASS" carried entirely by the DMA. */
    static const char text[] = "\r\nUARTDMA PASS via eDMA\r\n";
    for (i = 0; i < MSGLEN; i++) {
        msg[i] = (i < (int)sizeof(text) - 1) ? (uint8_t)text[i] : (uint8_t)' ';
    }

    LP_CTRL = CTRL_TE;                      /* transmitter on               */

    /* --- eDMA: memory -> LPUART DATA, ONE BYTE per request --------------- */
    TCD_SADDR(CHAN)  = (uint32_t)(uintptr_t)msg;
    TCD_SOFF(CHAN)   = 1;                   /* walk the string              */
    TCD_ATTR(CHAN)   = ATTR_8BIT;
    TCD_NBYTES(CHAN) = 1;                   /* one byte per DMA request     */
    TCD_SLAST(CHAN)  = (uint32_t)(-(int32_t)MSGLEN);
    TCD_DADDR(CHAN)  = LP_DATA_ADDR;        /* the data register            */
    TCD_DOFF(CHAN)   = 0;
    TCD_DLAST(CHAN)  = 0;
    TCD_CITER(CHAN)  = MSGLEN;
    TCD_BITER(CHAN)  = MSGLEN;
    TCD_CSR(CHAN)    = TCD_DREQ;            /* stop asking when done        */
    CH_MUX(CHAN)     = DMAREQ_FC4_TX;       /* listen to FlexComm4 TX       */

    CH_CSR(CHAN) = CSR_ERQ;                 /* HARDWARE requests enabled    */

    /*
     * Arming TDMAE is the moment the transmitter starts asking.  From here the
     * CPU does NOTHING — every byte is carried to the UART by the DMA.
     */
    LP_BAUD = BAUD_TDMAE;

    for (d = 0; d < 20000000 && !(CH_CSR(CHAN) & CSR_DONE); d++) {
    }

    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
