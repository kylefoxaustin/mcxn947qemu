/*
 * MCXN947 PowerQuad (DSP coprocessor) COMPUTE + compute-done NVIC interrupt test.
 *
 * Part 1 — real matrix-engine compute over MMIO (OUTBASE/INABASE/INBBASE/LENGTH/
 * CONTROL).  Verifies PowerQuad returns CORRECT results, not a stale/garbage
 * OUTBASE.  float32, checked as IEEE-754 bit patterns (no FPU needed):
 *   2x2 * 2x2 multiply [[1,2],[3,4]]*[[5,6],[7,8]] = [[19,22],[43,50]]
 *   2x2 element add    [[1,2],[3,4]]+[[5,6],[7,8]] = [[6,8],[10,12]]
 * Part 2 — completion interrupt: INTREN + NVIC IRQ 76, launch, ISR acks.
 * Prints "POWERQUAD PASS" when compute is correct AND the interrupt is delivered.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

#define PQ 0x400BF000u
#define PQ_OUTBASE   (*(volatile uint32_t *)(PQ + 0x00))
#define PQ_OUTFORMAT (*(volatile uint32_t *)(PQ + 0x04))
#define PQ_INABASE   (*(volatile uint32_t *)(PQ + 0x10))
#define PQ_INAFORMAT (*(volatile uint32_t *)(PQ + 0x14))
#define PQ_INBBASE   (*(volatile uint32_t *)(PQ + 0x18))
#define PQ_INBFORMAT (*(volatile uint32_t *)(PQ + 0x1C))
#define PQ_CONTROL  (*(volatile uint32_t *)(PQ + 0x100))
#define PQ_LENGTH   (*(volatile uint32_t *)(PQ + 0x104))
#define PQ_INTREN   (*(volatile uint32_t *)(PQ + 0x190))
#define PQ_INTRSTAT (*(volatile uint32_t *)(PQ + 0x198))

#define INTR_EN   (1u << 0)
#define INTR_STAT (1u << 0)
#define FMT_FLOAT 0x20u                          /* type(2=float) << 4 */
#define MTX_LEN(r1, c1, c2) ((r1) | ((c1) << 8) | ((c2) << 16))
#define CTRL(cp, op) (((cp) << 4) | (op))
#define CP_MTX 1u
#define OP_MULT 2u
#define OP_ADD  3u

/* IEEE-754 float32 bit patterns for the operands + expected results. */
#define F1 0x3F800000u
#define F2 0x40000000u
#define F3 0x40400000u
#define F4 0x40800000u
#define F5 0x40A00000u
#define F6 0x40C00000u
#define F7 0x40E00000u
#define F8 0x41000000u
#define F10 0x41200000u
#define F12 0x41400000u
#define F19 0x41980000u
#define F22 0x41B00000u
#define F43 0x422C0000u
#define F50 0x42480000u

#define MA 0x20002000u
#define MB 0x20002100u
#define MC 0x20002200u

#define NVIC_ISER2 (*(volatile uint32_t *)0xE000E108u)  /* IRQ 64..95 */
#define PQ_IRQ 76

static void putc_(char c)
{
    while (!(LP_STAT & STAT_TDRE)) {
    }
    LP_DATA = (uint8_t)c;
}

static void puts_(const char *s)
{
    while (*s) {
        putc_(*s++);
    }
}

static volatile uint32_t irq_count;

void pq_handler(void)
{
    if (PQ_INTRSTAT & INTR_STAT) {
        irq_count++;
        PQ_INTRSTAT = INTR_STAT;   /* write-1-to-clear */
    }
}

static int compute_ok(void)
{
    volatile uint32_t *a = (volatile uint32_t *)MA;
    volatile uint32_t *b = (volatile uint32_t *)MB;
    volatile uint32_t *c = (volatile uint32_t *)MC;

    a[0] = F1; a[1] = F2; a[2] = F3; a[3] = F4;
    b[0] = F5; b[1] = F6; b[2] = F7; b[3] = F8;
    PQ_INAFORMAT = FMT_FLOAT; PQ_INBFORMAT = FMT_FLOAT; PQ_OUTFORMAT = FMT_FLOAT;

    c[0] = c[1] = c[2] = c[3] = 0;
    PQ_OUTBASE = MC; PQ_INABASE = MA; PQ_INBBASE = MB;
    PQ_LENGTH = MTX_LEN(2, 2, 2);
    PQ_CONTROL = CTRL(CP_MTX, OP_MULT);
    if (c[0] != F19 || c[1] != F22 || c[2] != F43 || c[3] != F50) {
        return 0;
    }

    c[0] = c[1] = c[2] = c[3] = 0;
    PQ_OUTBASE = MC; PQ_INABASE = MA; PQ_INBBASE = MB;
    PQ_LENGTH = MTX_LEN(2, 2, 0);
    PQ_CONTROL = CTRL(CP_MTX, OP_ADD);
    if (c[0] != F6 || c[1] != F8 || c[2] != F10 || c[3] != F12) {
        return 0;
    }
    return 1;
}

void cpu0_main(void)
{
    int ccok;

    LP_CTRL = CTRL_TE;
    puts_("POWERQUAD test\r\n");

    ccok = compute_ok();

    PQ_INTREN = INTR_EN;
    NVIC_ISER2 = (1u << (PQ_IRQ - 64));
    __asm__ volatile ("cpsie i");

    /* Launch a (CP_PQ) instruction: retires + raises completion. */
    PQ_CONTROL = 0x00000001;

    while (irq_count < 1) {
    }

    puts_((ccok && irq_count >= 1) ? "POWERQUAD PASS\r\n" : "POWERQUAD FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[100] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + PQ_IRQ] = pq_handler,  /* exception 92 = IRQ 76 */
};
