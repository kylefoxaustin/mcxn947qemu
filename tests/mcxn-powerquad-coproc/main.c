/*
 * MCXN947 PowerQuad coprocessor (CP0) scalar-math test.
 *
 * The PowerQuad's scalar transcendentals and division are driven through a
 * custom Arm coprocessor (CP_PQ = p0) via MCR/MCRR and read back via MRC.
 * QEMU's Cortex-M33 does not implement CP0, so on stock QEMU these ops take a
 * NOCP UsageFault -> HardFault.  This image exercises the gated PowerQuad
 * coprocessor (enabled on the MCXN947 M33 via the "powerquad" CPU property):
 * each op feeds an IEEE-754 float32 bit pattern and checks the result's exact
 * bits, so no guest FPU/float codegen is needed — the coprocessor does the math.
 *
 * Prints "PQCP PASS" iff every op returns the expected result.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

/* CPACR (SCB): grant full access to CP0 (PowerQuad), as the SDK's PQ_Init does. */
#define CPACR (*(volatile uint32_t *)0xE000ED88u)

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

/*
 * Scalar op: MCR p0,<opc1>,Rt,c0,c0,<opc2> computes f(x) into COMP0; the
 * following MRC p0,0,Rt,c0,c0,0 reads the float32 result back.  CRn=c0 selects
 * COMP0 + float32 format.  opc1/opc2 must be literals in the MCR encoding.
 */
#define PQ_SCALAR(name, OPC1, OPC2)                                        \
static inline uint32_t name(uint32_t in)                                   \
{                                                                          \
    uint32_t o;                                                            \
    __asm__ volatile("mcr p0," #OPC1 ",%0,c0,c0," #OPC2 "\n\t"            \
                     "mrc p0,0,%0,c0,c0,0"                                 \
                     : "=r"(o) : "0"(in));                                 \
    return o;                                                              \
}

PQ_SCALAR(pq_inv,  0, 0)   /* PQ_INV   / PQ_TRANS */
PQ_SCALAR(pq_ln,   1, 0)   /* PQ_LN    / PQ_TRANS */
PQ_SCALAR(pq_sqrt, 2, 0)   /* PQ_SQRT  / PQ_TRANS */
PQ_SCALAR(pq_etox, 4, 0)   /* PQ_ETOX  / PQ_TRANS */
PQ_SCALAR(pq_sin,  0, 1)   /* PQ_SIN   / PQ_TRIG  */
PQ_SCALAR(pq_cos,  1, 1)   /* PQ_COS   / PQ_TRIG  */

/* Division: MCRR p0,0,Rt(=x2 denom),Rt2(=x1 numer),c6(PQ_DIV); result = x1/x2. */
static inline uint32_t pq_div(uint32_t x1, uint32_t x2)
{
    uint32_t o;
    __asm__ volatile("mcrr p0,0,%1,%2,c6\n\t"
                     "mrc p0,0,%0,c0,c0,0"
                     : "=r"(o) : "r"(x2), "r"(x1));
    return o;
}

/* Zero-init (.bss) and seeded in cpu0_main: there is no startup .data copy, so
 * an initialised writable global would read as zero.  See link.ld. */
static int checks_ok;

static void check(const char *name, uint32_t got, uint32_t want)
{
    puts_(name);
    if (got == want) {
        puts_(" ok\r\n");
    } else {
        puts_(" BAD\r\n");
        checks_ok = 0;
    }
}

/* IEEE-754 float32 bit patterns. */
#define F_0_0   0x00000000u   /* 0.0  */
#define F_0_25  0x3E800000u   /* 0.25 */
#define F_1_0   0x3F800000u   /* 1.0  */
#define F_2_0   0x40000000u   /* 2.0  */
#define F_3_0   0x40400000u   /* 3.0  */
#define F_4_0   0x40800000u   /* 4.0  */
#define F_6_0   0x40C00000u   /* 6.0  */

void cpu0_main(void)
{
    checks_ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("PQCP test\r\n");

    CPACR |= 0x3u;   /* CP0 full access (PowerQuad) */
    __asm__ volatile("dsb; isb");

    /* All cases chosen so the true result is exact in float32 -> exact bits. */
    check("sqrt(4)=2",  pq_sqrt(F_4_0),       F_2_0);
    check("inv(4)=.25", pq_inv(F_4_0),        F_0_25);
    check("sin(0)=0",   pq_sin(F_0_0),        F_0_0);
    check("cos(0)=1",   pq_cos(F_0_0),        F_1_0);
    check("ln(1)=0",    pq_ln(F_1_0),         F_0_0);
    check("etox(0)=1",  pq_etox(F_0_0),       F_1_0);
    check("6/2=3",      pq_div(F_6_0, F_2_0), F_3_0);
    check("1/4=.25",    pq_div(F_1_0, F_4_0), F_0_25);

    if (checks_ok) {
        puts_("PQCP PASS\r\n");
    } else {
        puts_("PQCP FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[16] = {
    [0] = (vec_t)0x20010000u,  /* initial MSP */
    [1] = cpu0_main,           /* Reset_Handler */
};
