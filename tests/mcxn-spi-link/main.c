/*
 * MCXN947 SPI board-to-board link test (inter-QEMU LPSPI over a chardev).
 *
 * Proves the MCX is an SPI b2b node interoperable with the fleet's `spi-link`
 * transport (91's hw/ssi/spi_link.c, ported here): FlexComm5 in LPSPI master
 * mode drives a QEMU SSI bus ("mcxn-lpspi"); a `-device spi-link,bus=mcxn-lpspi,
 * chardev=<sock>` bridges that bus to a socket, so each SPI transfer shifts one
 * byte to the peer (MOSI) and shifts one back from the peer (MISO), each
 * direction a FIFO-buffered byte stream (spi-link is data-path, not cycle-clock
 * accurate).
 *
 * cpu0 clocks a stream of 0x5A bytes out (MOSI — the peer verifies it) while
 * clocking the peer's framed pattern in (MISO): sync on a 0xA5 marker, then
 * collect 32 payload bytes and check them.  Prints "SPI LINK PASS <n>" once the
 * peer's pattern round-trips in.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

/* FlexComm4 / LPUART4 console. */
#define LP4_BASE 0x400B4000u
#define LP4_STAT (*(volatile uint32_t *)(LP4_BASE + 0x14))
#define LP4_CTRL (*(volatile uint32_t *)(LP4_BASE + 0x18))
#define LP4_DATA (*(volatile uint32_t *)(LP4_BASE + 0x1C))
#define CTRL_TE  (1u << 19)
#define STAT_TDRE (1u << 23)

/* FlexComm5 @ 0x400B5000 as an LPSPI master (SSI-bus "mcxn-lpspi"). */
#define FC5_BASE  0x400B5000u
#define SPI_CR    (*(volatile uint32_t *)(FC5_BASE + 0x10))
#define SPI_SR    (*(volatile uint32_t *)(FC5_BASE + 0x14))
#define SPI_CFGR1 (*(volatile uint32_t *)(FC5_BASE + 0x24))
#define SPI_TCR   (*(volatile uint32_t *)(FC5_BASE + 0x60))
#define SPI_TDR   (*(volatile uint32_t *)(FC5_BASE + 0x64))
#define SPI_RDR   (*(volatile uint32_t *)(FC5_BASE + 0x74))
#define SPI_PSELID (*(volatile uint32_t *)(FC5_BASE + 0xFF8))
#define PERSEL_LPSPI     2u
#define SPI_CR_MEN       0x1u
#define SPI_SR_RDF       0x2u
#define SPI_CFGR1_MASTER 0x1u
#define TCR_FRAMESZ_8    7u          /* (framesz-1); 8-bit words */

#define MOSI_BYTE 0x5Au              /* stream we clock out for the peer */
#define MARKER    0xA5u              /* frame marker from the peer       */
#define IDLE      0xFFu              /* spi-link returns this when empty */
#define N 32

static void c_putc(char c) { while (!(LP4_STAT & STAT_TDRE)) {} LP4_DATA = (uint8_t)c; }
static void c_puts(const char *s) { while (*s) { c_putc(*s++); } }
static void c_putdec(uint32_t v)
{
    char b[11]; int n = 0;
    if (!v) { c_putc('0'); return; }
    while (v) { b[n++] = '0' + (v % 10); v /= 10; }
    while (n) { c_putc(b[--n]); }
}

static uint8_t expect(int i) { return (uint8_t)((i * 3 + 5) & 0x7F); }

static uint8_t spi_xfer(uint8_t mosi)
{
    SPI_TDR = mosi;                  /* shift one byte out + one in */
    return (uint8_t)SPI_RDR;         /* MISO from the peer (or 0xFF idle) */
}

static void delay(uint32_t n) { for (volatile uint32_t i = 0; i < n; i++) { } }

void cpu0_main(void)
{
    LP4_CTRL = CTRL_TE;
    c_puts("SPI LINK test\r\n");

    /* FlexComm5 -> LPSPI master, 8-bit frames. */
    SPI_PSELID = PERSEL_LPSPI;
    SPI_CFGR1  = SPI_CFGR1_MASTER;
    SPI_TCR    = TCR_FRAMESZ_8;
    SPI_CR     = SPI_CR_MEN;

    int state = 0;                   /* 0 = seeking marker; 1..N = collecting */
    int got = 0;
    uint32_t clocks = 0;

    while (state <= N && got < N && clocks < 4000000u) {
        clocks++;
        uint8_t miso = spi_xfer(MOSI_BYTE);
        if (miso == IDLE) {
            delay(20000);            /* FIFO empty: pace, don't flood MOSI */
            continue;
        }
        if (state == 0) {
            if (miso == MARKER) {
                state = 1;
            }
        } else {
            int idx = state - 1;
            if (miso == expect(idx)) {
                state++;
                if (state > N) {
                    got = N;
                }
            } else {
                state = (miso == MARKER) ? 1 : 0;   /* resync */
            }
        }
    }

    if (got == N) {
        c_puts("SPI LINK PASS "); c_putdec(got); c_puts("\r\n");
        /*
         * Keep clocking after PASS so a peer that busy-loops sending frames
         * always drains: the spi-link RX FIFO is finite (256 deep), and if we
         * stopped popping it, backpressure would block the peer's chardev write
         * -> its LPSPI TDR write -> a hung spidev ioctl (fleet lesson from the
         * imx93<->MCX SPI lab).  A bounded byte count keeps the link re-runnable.
         */
        for (;;) {
            spi_xfer(MOSI_BYTE);
            delay(2000);
        }
    } else {
        c_puts("SPI LINK FAIL state="); c_putdec((uint32_t)state);
        c_puts(" clocks="); c_putdec(clocks); c_puts("\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[48] = {
    [0]  = (vec_t)0x20010000u,
    [1]  = cpu0_main,
};
