/*
 * MCXN947 GPIO functional smoke test.
 *
 * Drives GPIO0 as outputs and exercises PDOR / PSOR / PCOR / PTOR, reading
 * back PDOR and PDIR after each step.  Prints "GPIO PASS" over LPUART4 if every
 * readback matches, else "GPIO FAIL".
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

#define GPIO0_BASE 0x40096000u
#define GPIO_PDOR (*(volatile uint32_t *)(GPIO0_BASE + 0x40))
#define GPIO_PSOR (*(volatile uint32_t *)(GPIO0_BASE + 0x44))
#define GPIO_PCOR (*(volatile uint32_t *)(GPIO0_BASE + 0x48))
#define GPIO_PTOR (*(volatile uint32_t *)(GPIO0_BASE + 0x4C))
#define GPIO_PDIR (*(volatile uint32_t *)(GPIO0_BASE + 0x50))
#define GPIO_PDDR (*(volatile uint32_t *)(GPIO0_BASE + 0x54))

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

void cpu0_main(void)
{
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("GPIO test\r\n");

    GPIO_PDDR = 0xFF;          /* pins 0..7 outputs */

    GPIO_PSOR = 0x05;          /* set pins 0 and 2 */
    ok &= (GPIO_PDOR == 0x05);
    ok &= (GPIO_PDIR == 0x05); /* output pins read back their level */

    GPIO_PCOR = 0x01;          /* clear pin 0 */
    ok &= (GPIO_PDOR == 0x04);

    GPIO_PTOR = 0x04;          /* toggle pin 2 -> cleared */
    ok &= (GPIO_PDOR == 0x00);

    GPIO_PTOR = 0x02;          /* toggle pin 1 -> set */
    ok &= (GPIO_PDIR == 0x02);

    puts_(ok ? "GPIO PASS\r\n" : "GPIO FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[] = {
    (vec_t)0x20010000u,        /* initial MSP */
    cpu0_main,                 /* Reset_Handler (thumb bit set by toolchain) */
};
