/*
 * MCXN947 FMU flash erase/program smoke test.
 *
 * Programs a flash word (direct write to the RAM-backed flash), then uses the
 * FMU Erase Sector command and verifies the sector reads back erased (0xFF).
 * Prints "FMU PASS" if the program landed and the FMU erase worked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t*)(LP+0x14))
#define LP_CTRL (*(volatile uint32_t*)(LP+0x18))
#define LP_DATA (*(volatile uint32_t*)(LP+0x1C))
static void putc_(char c){ while(!(LP_STAT&(1u<<23))){} LP_DATA=(uint8_t)c; }
static void puts_(const char*s){ while(*s) putc_(*s++); }

#define FMU 0x40043000u
#define FMU_FSTAT   (*(volatile uint32_t*)(FMU+0x0))
#define FMU_FCCOB0  (*(volatile uint32_t*)(FMU+0x10))
#define FMU_FCCOB1  (*(volatile uint32_t*)(FMU+0x14))
#define CCIF 0x80u
#define FAIL 0x1u
#define FLASH_TEST 0x10010000u   /* a flash sector well past this test's code */

void cpu0_main(void)
{
    volatile uint32_t *p = (volatile uint32_t *)FLASH_TEST;
    int ok = 1;
    LP_CTRL = (1u<<19);
    puts_("FMU test\r\n");

    *p = 0x12345678u;                 /* program (direct write to RAM-flash) */
    ok &= (*p == 0x12345678u);

    while (!(FMU_FSTAT & CCIF)) {}     /* FMU idle */
    FMU_FCCOB0 = 0x42;                /* Erase Sector */
    FMU_FCCOB1 = FLASH_TEST;
    FMU_FSTAT = CCIF;                 /* launch */
    while (!(FMU_FSTAT & CCIF)) {}     /* wait complete */

    ok &= (*p == 0xFFFFFFFFu);        /* sector erased */
    ok &= !(FMU_FSTAT & FAIL);

    puts_(ok ? "FMU PASS\r\n" : "FMU FAIL\r\n");
    for (;;) {}
}
typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
