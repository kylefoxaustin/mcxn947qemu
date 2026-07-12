/*
 * MCXN947 SECURE ALIAS sweep — the axis nobody ever varied.
 *
 * TrustZone-M aliases every peripheral twice: non-secure at 0x400x_xxxx and
 * secure at 0x500x_xxxx (+0x1000_0000).  The SoC creates 34 such aliases.
 *
 * ⭐ AND NOT ONE TEST HAD EVER TOUCHED ANY OF THEM.  Every peripheral test in this
 * tree drives the NON-SECURE alias only.  So if an alias were mis-mapped — wrong
 * base, wrong size, or pointing at the WRONG DEVICE — the entire suite would stay
 * green, while TrustZone-secure firmware (TF-M, NXP secure boot: the NORMAL case
 * on MCX N) talked to the wrong peripheral or to nothing at all.
 *
 * Found by applying ollama_95_neutron's rule to my own tree:
 *
 *     "I swept K exhaustively.  M exhaustively.  N exhaustively.  AND EVERY ONE OF
 *      THOSE SWEEPS RAN ON ONE MODEL'S WEIGHTS.  You cannot find a model-dependent
 *      constant by varying anything except the model.
 *      THE RIGOUR ON ONE AXIS IS THE CAMOUFLAGE ON THE AXIS YOU NEVER NAMED."
 *
 * I had swept prescalers, filter orders, matrix shapes and access widths.  I had
 * never once swept the SECURITY ALIAS.
 *
 * What must hold, for every aliased peripheral:
 *   1. the SAME read-only identity register reads IDENTICALLY through both views;
 *   2. a write through ONE view is visible through the OTHER — they are the SAME
 *      DEVICE, not two copies.  (A model that mapped a fresh MemoryRegion at the
 *      secure base would pass check 1 and FAIL check 2.)
 *   3. the alias is at exactly +0x1000_0000.
 *
 * Prints "SECALIAS PASS" only if every peripheral holds.
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

static void puthex(uint32_t v)
{
    const char *H = "0123456789ABCDEF";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        putc_(H[(v >> i) & 0xF]);
    }
}

#define SECURE_ALIAS 0x10000000u

#define RD(a)     (*(volatile uint32_t *)(uintptr_t)(a))
#define WR(a, v)  (*(volatile uint32_t *)(uintptr_t)(a) = (v))

/*
 * Peripherals to sweep, with a register that is safe to READ and one that is
 * safe to WRITE-then-restore.  ident_off is a read-only/stable register; rw_off
 * is a plain read/write register that no side effect keys off.
 */
struct periph {
    const char *name;
    uint32_t    base;      /* non-secure */
    uint32_t    ident_off; /* stable register, read through both views */
    uint32_t    rw_off;    /* scratch register, written through one view */
};

static const struct periph periphs[] = {
    { "lpuart4", 0x400B4000u, 0x00,  0x10 },  /* VERID   / BAUD      */
    { "flexcan0",0x400D4000u, 0x00,  0x008 }, /* MCR     / TIMER     */
    { "sai0",    0x40106000u, 0x00,  0x0C },  /* VERID   / TCR1      */
    { "dac0",    0x4010F000u, 0x00,  0x10 },  /* VERID   / FCR       */
    { "pwm0",    0x400B8000u, 0x02,  0x0E },  /* SM0 INIT/ VAL1      */
    { "edma0",   0x40080000u, 0x00,  0x00 },  /* MP_CSR  / MP_CSR    */
    { "usdhc0",  0x40109000u, 0x40,  0x08 },  /* CAPS    / CMD_ARG   */
    { "i3c0",    0x40021000u, 0x00,  0x00 },  /* MCONFIG / MCONFIG   */
};

#define NPERIPH (sizeof(periphs) / sizeof(periphs[0]))

void cpu0_main(void)
{
    int ok = 1;
    unsigned i;

    LP_CTRL = CTRL_TE;
    puts_("secure-alias sweep\r\n");

    for (i = 0; i < NPERIPH; i++) {
        const struct periph *p = &periphs[i];
        uint32_t ns = p->base;
        uint32_t s  = p->base + SECURE_ALIAS;
        uint32_t a, b, saved, probe;
        int this_ok = 1;

        /* --- 1: the same identity register through both views ------------- */
        a = RD(ns + p->ident_off);
        b = RD(s  + p->ident_off);
        this_ok &= (a == b);

        /* --- 2: SAME DEVICE, not a copy.  Write NS, read back through S. --- */
        saved = RD(ns + p->rw_off);
        probe = saved ^ 0x00000005u;        /* flip low bits, nothing keys off */
        WR(ns + p->rw_off, probe);
        this_ok &= (RD(s + p->rw_off) == RD(ns + p->rw_off));

        /* ...and the other direction: write S, read back through NS. */
        WR(s + p->rw_off, saved);
        this_ok &= (RD(ns + p->rw_off) == RD(s + p->rw_off));

        WR(ns + p->rw_off, saved);          /* restore */

        puts_(this_ok ? "  ok   " : "  FAIL ");
        puts_(p->name);
        puts_("  ns=");   puthex(a);
        puts_(" s=");     puthex(b);
        puts_("\r\n");
        ok &= this_ok;
    }

    puts_(ok ? "SECALIAS PASS\r\n" : "SECALIAS FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
