/*
 * MCXN947 QSPI execute-in-place BOOT — the M33 resets and runs from the external FlexSPI NOR.
 *
 * There is NO internal-flash image: the vector table (initial SP + reset PC), the FlexSPI
 * Config Block (FCB, tag "FCFB" at 0x9000_0400), and all the code live in the FlexSPI XIP NOR
 * (secure alias 0x9000_0000).  On `-machine frdm-mcxn947,qspi-boot=on` the core resets from
 * that window (modelling the state after the boot ROM validated the FCB, configured FlexSPI,
 * and jumped),
 * and every instruction below is FETCHED IN PLACE from QSPI -- if XIP boot did not work, the
 * CPU would fetch garbage at reset and never reach this banner.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LP + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LP + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LP + 0x1C))
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

/*
 * The FlexSPI Config Block at NOR offset 0x400 (0x9000_0400).  The boot ROM reads its tag ("FCFB",
 * FLEXSPI_CFG_BLK_TAG = 0x42464346) to validate the image before booting.  The rest of the
 * 512-byte block (lookup table, read/timing settings) is what a real ROM parses to configure
 * FlexSPI; here the AHB window is already a live NOR mirror, so only the tag is checked.
 */
__attribute__((section(".fcb"), used))
const uint32_t flexspi_fcb[128] = { 0x42464346u };   /* tag; remainder zero */

/* Sum-of-squares -- real work the compiler cannot fold away, fetched in place from QSPI. */
static uint32_t work(uint32_t n)
{
    uint32_t s = 0, i;

    for (i = 1; i <= n; i++) {
        s += i * i;
    }
    return s;
}

void cpu0_main(void)
{
    uint32_t r;

    LP_CTRL = (1u << 19);
    puts_("QSPI-BOOT test\r\n");

    /* Prove the code below (also in QSPI) really executes: 1^2+..+10^2 = 385. */
    r = work(10);

    puts_((r == 385u) ? "QSPI-BOOT PASS\r\n" : "QSPI-BOOT FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
