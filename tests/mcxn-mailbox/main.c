/*
 * MCXN947 inter-CPU MAILBOX cross-core interrupt test.
 *
 * cpu0 boots, releases cpu1 (SYSCON CPUCTRL/CPBOOT, as in mcxn-dualcore), then
 * the two cores exchange mailbox interrupts both ways:
 *
 *   cpu0 --IRQSET[1]--> cpu1 mailbox IRQ (54 on cpu1's NVIC)
 *   cpu1 --IRQSET[0]--> cpu0 mailbox IRQ (54 on cpu0's NVIC)
 *
 * Each CPU's mailbox IRQ word (MBOXIRQ[n].IRQ), when non-zero, asserts IRQ 54 on
 * that core only.  cpu0 pings cpu1; cpu1's handler clears its word and pongs
 * back; cpu0's handler sees the pong.  Prints "MAILBOX PASS" once the round-trip
 * completes through both NVICs — proving a genuine cross-core interrupt.
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

/* MAILBOX: MBOXIRQ[0] (cpu0) @ 0x00, MBOXIRQ[1] (cpu1) @ 0x10. */
#define MB_BASE        0x400B2000u
#define MB0_IRQ        (*(volatile uint32_t *)(MB_BASE + 0x00))
#define MB0_IRQSET     (*(volatile uint32_t *)(MB_BASE + 0x04))
#define MB0_IRQCLR     (*(volatile uint32_t *)(MB_BASE + 0x08))
#define MB1_IRQ        (*(volatile uint32_t *)(MB_BASE + 0x10))
#define MB1_IRQSET     (*(volatile uint32_t *)(MB_BASE + 0x14))
#define MB1_IRQCLR     (*(volatile uint32_t *)(MB_BASE + 0x18))

#define MAILBOX_IRQ    54
#define NVIC_ISER1     (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */

#define PING_MSG       0x00000001u
#define PONG_MSG       0x00000002u

/* Shared SRAM cells (link.ld discards .data/.bss; use fixed addresses for
 * cross-core state and initialise them from cpu0 before releasing cpu1). */
#define READY     (*(volatile uint32_t *)0x20001000u)   /* cpu1 -> cpu0: I'm armed */
#define GOT_PONG  (*(volatile uint32_t *)0x20001004u)   /* cpu0 handler -> cpu0 main */
#define READY_VAL 0x5A5A0001u

#define CPU1_VT   0x20002000u           /* cpu1 vector table (SRAM) */
#define CPU1_SP   0x20020000u

static void uart_putc(char c)
{
    while (!(LPUART_STAT & STAT_TDRE)) {
    }
    LPUART_DATA = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

/* cpu0's mailbox ISR: cpu1 pinged us back. */
void cpu0_mb_handler(void)
{
    uint32_t v = MB0_IRQ;
    MB0_IRQCLR = v;          /* clear -> deassert cpu0 line */
    if (v == PONG_MSG) {
        GOT_PONG = 1;
    }
}

/* cpu1's mailbox ISR: cpu0 pinged us; pong back. */
void cpu1_mb_handler(void)
{
    uint32_t v = MB1_IRQ;
    MB1_IRQCLR = v;          /* clear -> deassert cpu1 line */
    if (v == PING_MSG) {
        MB0_IRQSET = PONG_MSG;   /* interrupt cpu0 */
    }
}

void cpu1_main(void)
{
    NVIC_ISER1 = (1u << (MAILBOX_IRQ - 32));   /* cpu1's own NVIC */
    __asm__ volatile ("cpsie i");
    READY = READY_VAL;                          /* tell cpu0 we're armed */
    for (;;) {
    }
}

void cpu0_main(void)
{
    volatile uint32_t *vt = (volatile uint32_t *)CPU1_VT;
    uint32_t spins = 0;

    LPUART_CTRL = CTRL_TE;
    uart_puts("MAILBOX test\r\n");

    READY = 0;
    GOT_PONG = 0;

    /* Build cpu1's vector table: SP, reset PC, and the mailbox handler at 54. */
    vt[0] = CPU1_SP;
    vt[1] = ((uint32_t)&cpu1_main) | 1u;
    vt[16 + MAILBOX_IRQ] = ((uint32_t)&cpu1_mb_handler) | 1u;

    /* cpu0's own mailbox IRQ. */
    NVIC_ISER1 = (1u << (MAILBOX_IRQ - 32));
    __asm__ volatile ("cpsie i");

    /* Release cpu1. */
    SYSCON_CPBOOT  = CPU1_VT;
    SYSCON_CPUCTRL = PROT_KEY | CPU1CLKEN;

    /* Wait until cpu1 has armed its NVIC, then ping it. */
    while (READY != READY_VAL) {
    }
    MB1_IRQSET = PING_MSG;       /* interrupt cpu1 */

    /* Wait for cpu1's pong to reach our handler (bounded). */
    while (!GOT_PONG && ++spins < 100000000u) {
    }

    uart_puts(GOT_PONG ? "MAILBOX PASS\r\n" : "MAILBOX FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t cpu0_vectors[16 + MAILBOX_IRQ + 1] = {
    [0]  = (vec_t)0x20010000u,        /* cpu0 initial MSP */
    [1]  = cpu0_main,                 /* cpu0 reset handler */
    [16 + MAILBOX_IRQ] = cpu0_mb_handler,
};
