/*
 * MCXN947 dual-core rpmsg-style shared-memory IPC test.
 *
 * Proves the OpenAMP/rpmsg *transport substrate* end-to-end: real message
 * payloads carried through shared-SRAM ring buffers (a simplified vring), with
 * the inter-CPU MAILBOX as the doorbell — exactly what a developer's OpenAMP
 * rpmsg code does (this model provides the pieces: dual M33 + shared SRAM +
 * cross-core mailbox IRQ; the mailbox test alone only carried a fixed notify
 * value, not arbitrary data through a ring).
 *
 *   cpu0 (master)  --push N msgs to TX ring--> [MAILBOX kick] --> cpu1 (remote)
 *   cpu1 drains TX, transforms each payload, pushes replies to RX ring, kicks
 *   cpu0 --> cpu0 drains RX + verifies every payload round-tripped correctly.
 *
 * Runs ROUNDS batches of MSGS_PER_ROUND to exercise the ring across multiple
 * doorbells.  Prints "RPMSG PASS <n>" once all messages verify.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE   0x400B4000u
#define LPUART_STAT    (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LPUART_CTRL    (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LPUART_DATA    (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE        (1u << 19)
#define STAT_TDRE      (1u << 23)

#define SYSCON_BASE    0x40000000u
#define SYSCON_CPUCTRL (*(volatile uint32_t *)(SYSCON_BASE + 0x800))
#define SYSCON_CPBOOT  (*(volatile uint32_t *)(SYSCON_BASE + 0x804))
#define CPU1CLKEN      (1u << 3)
#define PROT_KEY       0xC0C40000u

#define MB_BASE        0x400B2000u
#define MB0_IRQ        (*(volatile uint32_t *)(MB_BASE + 0x00))   /* cpu0 word */
#define MB0_IRQSET     (*(volatile uint32_t *)(MB_BASE + 0x04))
#define MB0_IRQCLR     (*(volatile uint32_t *)(MB_BASE + 0x08))
#define MB1_IRQ        (*(volatile uint32_t *)(MB_BASE + 0x10))   /* cpu1 word */
#define MB1_IRQSET     (*(volatile uint32_t *)(MB_BASE + 0x14))
#define MB1_IRQCLR     (*(volatile uint32_t *)(MB_BASE + 0x18))
#define DOORBELL       0x1u

#define MAILBOX_IRQ    54
#define NVIC_ISER1     (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */

/* --- Shared control block + rings in SRAM (link.ld discards .data/.bss; use
 *     fixed addresses; cpu0 zeroes them before releasing cpu1). --------------
 * Head = producer index (monotonic); Tail = consumer index.  ring depth is a
 * power of two; slot index = (idx & (SLOTS-1)).  Single-producer/single-
 * consumer per ring, so plain indices + a write barrier suffice. */
#define M32(a)   (*(volatile uint32_t *)(uintptr_t)(a))
#define TX_HEAD  M32(0x20001000u)   /* cpu0 -> cpu1 : cpu0 produces  */
#define TX_TAIL  M32(0x20001004u)   /* cpu0 -> cpu1 : cpu1 consumes  */
#define RX_HEAD  M32(0x20001008u)   /* cpu1 -> cpu0 : cpu1 produces  */
#define RX_TAIL  M32(0x2000100Cu)   /* cpu1 -> cpu0 : cpu0 consumes  */
#define READY    M32(0x20001010u)   /* cpu1 -> cpu0 : armed          */
#define VERIFIED M32(0x20001014u)   /* cpu0 handler -> cpu0 main     */
#define BADMSG   M32(0x20001018u)   /* cpu0 handler -> cpu0 main     */
#define READY_VAL 0x5A5A0001u

#define TX_RING  0x20001100u        /* SLOTS * 8 bytes {seq, val}    */
#define RX_RING  0x20001300u
#define SLOTS    8
#define SLOT(ring, idx)  ((ring) + (((idx) & (SLOTS - 1)) * 8u))

#define ROUNDS         4
#define MSGS_PER_ROUND 5            /* < SLOTS, so a round fits the ring */

#define CPU1_VT   0x20002000u
#define CPU1_SP   0x20020000u

/* rpmsg-ish payload transform the remote applies; the master checks it. */
static inline uint32_t transform(uint32_t seq, uint32_t val)
{
    return (val ^ 0xA5A5A5A5u) + (seq * 2654435761u);
}

static inline void dmb(void) { __asm__ volatile ("dmb ish" ::: "memory"); }

static void uart_putc(char c) { while (!(LPUART_STAT & STAT_TDRE)) {} LPUART_DATA = (uint8_t)c; }
static void uart_puts(const char *s) { while (*s) { uart_putc(*s++); } }
static void uart_putdec(uint32_t v)
{
    char b[11]; int n = 0;
    if (!v) { uart_putc('0'); return; }
    while (v) { b[n++] = '0' + (v % 10); v /= 10; }
    while (n) { uart_putc(b[--n]); }
}

/* ---- cpu1 (remote): drain TX ring, transform, produce to RX ring ---------- */
void cpu1_mb_handler(void)
{
    MB1_IRQCLR = MB1_IRQ;                    /* clear doorbell */
    int produced = 0;
    while (TX_TAIL != TX_HEAD) {
        uint32_t t = TX_TAIL;
        uint32_t s = M32(SLOT(TX_RING, t) + 0);
        uint32_t v = M32(SLOT(TX_RING, t) + 4);
        dmb();
        TX_TAIL = t + 1;                     /* release the TX slot */

        uint32_t rh = RX_HEAD;
        M32(SLOT(RX_RING, rh) + 0) = s;
        M32(SLOT(RX_RING, rh) + 4) = transform(s, v);
        dmb();
        RX_HEAD = rh + 1;                    /* publish the reply */
        produced++;
    }
    if (produced) {
        MB0_IRQSET = DOORBELL;               /* kick cpu0 */
    }
}

void cpu1_main(void)
{
    NVIC_ISER1 = (1u << (MAILBOX_IRQ - 32));
    __asm__ volatile ("cpsie i");
    READY = READY_VAL;
    for (;;) {
    }
}

/* ---- cpu0 (master): verify replies as they arrive ------------------------- */
void cpu0_mb_handler(void)
{
    MB0_IRQCLR = MB0_IRQ;
    while (RX_TAIL != RX_HEAD) {
        uint32_t r = RX_TAIL;
        uint32_t s = M32(SLOT(RX_RING, r) + 0);
        uint32_t v = M32(SLOT(RX_RING, r) + 4);
        dmb();
        RX_TAIL = r + 1;
        /* the master sent val = seq * 3 + 7 (see below), so recompute + check */
        if (v == transform(s, s * 3u + 7u)) {
            VERIFIED = VERIFIED + 1;
        } else {
            BADMSG = BADMSG + 1;
        }
    }
}

void cpu0_main(void)
{
    volatile uint32_t *vt = (volatile uint32_t *)CPU1_VT;

    LPUART_CTRL = CTRL_TE;
    uart_puts("RPMSG test\r\n");

    TX_HEAD = TX_TAIL = RX_HEAD = RX_TAIL = 0;
    READY = VERIFIED = BADMSG = 0;

    vt[0] = CPU1_SP;
    vt[1] = ((uint32_t)&cpu1_main) | 1u;
    vt[16 + MAILBOX_IRQ] = ((uint32_t)&cpu1_mb_handler) | 1u;

    NVIC_ISER1 = (1u << (MAILBOX_IRQ - 32));
    __asm__ volatile ("cpsie i");

    SYSCON_CPBOOT  = CPU1_VT;
    SYSCON_CPUCTRL = PROT_KEY | CPU1CLKEN;   /* release cpu1 */
    while (READY != READY_VAL) {
    }

    uint32_t seq = 0, total = ROUNDS * MSGS_PER_ROUND;
    for (int round = 0; round < ROUNDS; round++) {
        for (int m = 0; m < MSGS_PER_ROUND; m++) {
            uint32_t h = TX_HEAD;
            M32(SLOT(TX_RING, h) + 0) = seq;
            M32(SLOT(TX_RING, h) + 4) = seq * 3u + 7u;   /* the payload */
            dmb();
            TX_HEAD = h + 1;
            seq++;
        }
        MB1_IRQSET = DOORBELL;                /* one doorbell per batch */
        /* wait until this round's replies have all verified (bounded spin) */
        uint32_t want = (uint32_t)(round + 1) * MSGS_PER_ROUND, spins = 0;
        while (VERIFIED + BADMSG < want && ++spins < 200000000u) {
        }
    }

    if (VERIFIED == total && BADMSG == 0) {
        uart_puts("RPMSG PASS "); uart_putdec(VERIFIED); uart_puts("\r\n");
    } else {
        uart_puts("RPMSG FAIL verified="); uart_putdec(VERIFIED);
        uart_puts(" bad="); uart_putdec(BADMSG); uart_puts("\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t cpu0_vectors[16 + MAILBOX_IRQ + 1] = {
    [0]  = (vec_t)0x20010000u,
    [1]  = cpu0_main,
    [16 + MAILBOX_IRQ] = cpu0_mb_handler,
};
