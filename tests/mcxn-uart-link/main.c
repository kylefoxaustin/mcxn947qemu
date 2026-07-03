/*
 * MCXN947 UART board-to-board link test (inter-QEMU LPUART over a chardev).
 *
 * Proves the MCX is a drop-in UART b2b node — the same pattern as the i.MX91
 * two-board LPUART link (91's run-uart.sh) and holobench's UART lab: a spare
 * FlexComm LPUART (FlexComm2 / LPUART2 @ 0x4009_4000, the cpu1-console index =
 * serial_hd(1)) is wired to a socket chardev, so a peer on the other end of the
 * socket exchanges raw bytes with it — no protocol, just a byte stream (QEMU
 * chardev sockets carry it directly; the LPUART model already does full-duplex
 * TX + RX-IRQ over the chardev).
 *
 * cpu0 sends a 32-byte payload out LPUART2, a peer echoes it back, the RX ISR
 * collects the echo and checks it byte-for-byte.  Prints "UART LINK PASS <n>"
 * on the FlexComm4 console once all bytes round-trip.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

/* FlexComm4 / LPUART4 = console (serial_hd(0)). */
#define LP4_BASE 0x400B4000u
#define LP4_STAT (*(volatile uint32_t *)(LP4_BASE + 0x14))
#define LP4_CTRL (*(volatile uint32_t *)(LP4_BASE + 0x18))
#define LP4_DATA (*(volatile uint32_t *)(LP4_BASE + 0x1C))

/* FlexComm2 / LPUART2 = board-to-board link (serial_hd(1)), IRQ 37. */
#define LP2_BASE 0x40094000u
#define LP2_STAT (*(volatile uint32_t *)(LP2_BASE + 0x14))
#define LP2_CTRL (*(volatile uint32_t *)(LP2_BASE + 0x18))
#define LP2_DATA (*(volatile uint32_t *)(LP2_BASE + 0x1C))
#define LP2_IRQ  37

#define CTRL_RE   0x00040000u
#define CTRL_TE   0x00080000u
#define CTRL_RIE  0x00200000u
#define STAT_RDRF 0x00200000u
#define STAT_TDRE 0x00800000u

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)   /* IRQ 32..63 */

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

static uint8_t txbuf[N];
static volatile uint8_t rxbuf[N];
static volatile int rxn;
static volatile int mismatch;
static volatile int done;

static uint8_t expect(int i) { return (uint8_t)(i * 7 + 3); }

void lp2_isr(void)
{
    while (LP2_STAT & STAT_RDRF) {          /* drain the RX holding register */
        uint8_t b = (uint8_t)LP2_DATA;
        if (rxn < N) {
            rxbuf[rxn] = b;
            if (b != expect(rxn)) {
                mismatch = 1;
            }
            rxn++;
            if (rxn == N) {
                done = 1;
            }
        }
    }
}

void cpu0_main(void)
{
    LP4_CTRL = CTRL_TE;
    c_puts("UART LINK test\r\n");

    rxn = 0;
    mismatch = 0;
    done = 0;

    /* Enable RX first and wait for the peer's "GO" byte before transmitting:
     * a socket chardev drops TX when no client is attached yet, so this
     * handshake makes the link robust to connect timing. */
    LP2_CTRL = CTRL_TE | CTRL_RE;
    while (!(LP2_STAT & STAT_RDRF)) {
    }
    (void)LP2_DATA;                          /* consume the GO byte */

    /* Now arm the RX interrupt + NVIC line and send the payload; the peer
     * echoes it back to our RX ISR. */
    LP2_CTRL = CTRL_TE | CTRL_RE | CTRL_RIE;
    NVIC_ISER1 = (1u << (LP2_IRQ - 32));
    __asm__ volatile ("cpsie i");

    /* Send the payload out LPUART2; the peer echoes it back to our RX ISR. */
    for (int i = 0; i < N; i++) {
        txbuf[i] = expect(i);
        while (!(LP2_STAT & STAT_TDRE)) {
        }
        LP2_DATA = txbuf[i];
    }

    /* Sleep on the RX interrupts until all N bytes have round-tripped (the
     * 1-byte RX holding register means each byte arrives via its own IRQ; WFI
     * waits real time regardless of TCG speed, run.sh's timeout is the backstop). */
    while (!done) {
        __asm__ volatile ("wfi");
    }

    if (rxn == N && !mismatch) {
        c_puts("UART LINK PASS "); c_putdec(rxn); c_puts("\r\n");
    } else {
        c_puts("UART LINK FAIL rxn="); c_putdec(rxn);
        c_puts(" mismatch="); c_putdec(mismatch); c_puts("\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[16 + LP2_IRQ + 1] = {
    [0]  = (vec_t)0x20010000u,
    [1]  = cpu0_main,
    [16 + LP2_IRQ] = lp2_isr,
};
