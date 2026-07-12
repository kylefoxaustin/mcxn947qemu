/*
 * MCXN947 FlexSPI execute-in-place (XIP) test.
 *
 * Proves the FlexSPI0 AHB-mapped NOR window is real, executable memory: the
 * vector table and startup live in internal flash (0x1000_0000) — exactly the
 * boot-ROM/internal-flash hand-off a real XIP app uses — while the application
 * routine `xip_compute` is linked into and executed FROM the FlexSPI AHB window
 * (secure view 0x9000_0000), with no copy to SRAM.
 *
 * The reset handler calls `xip_compute` through a function pointer (an indirect
 * BLX to the absolute 0x9000_0000 address), so the CPU fetches and runs those
 * instructions in place.  It also asserts the routine's address actually lies
 * in the QSPI window, then verifies the computed result.  Prints "XIP PASS"
 * when code ran in place and returned the right answer.
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

/* FlexSPI0 AHB-mapped NOR (XIP), secure view; see SoC MCXN_FLEXSPI0_AHB_S. */
#define QSPI_BASE 0x90000000u

/* FlexSPI IP path, used to interrogate the FLASH ITSELF (not the XIP mirror). */
#define FSPI 0x400C8000u
#define FSPI_INTR     (*(volatile uint32_t *)(FSPI+0x14))
#define FSPI_IPCR0    (*(volatile uint32_t *)(FSPI+0xA0))
#define FSPI_IPCR1    (*(volatile uint32_t *)(FSPI+0xA4))
#define FSPI_IPCMD    (*(volatile uint32_t *)(FSPI+0xB0))
#define FSPI_IPRXFCR  (*(volatile uint32_t *)(FSPI+0xB8))
#define FSPI_IPRXFSTS (*(volatile uint32_t *)(FSPI+0xF0))
#define FSPI_RFDR(i)  (*(volatile uint32_t *)(FSPI+0x100+(i)*4))
#define FSPI_LUT(i)   (*(volatile uint32_t *)(FSPI+0x200+(i)*4))
#define LUT_SEQ(c0,p0,o0,c1,p1,o1) \
    ((uint32_t)(o0)|((uint32_t)(p0)<<8)|((uint32_t)(c0)<<10)| \
     ((uint32_t)(o1)<<16)|((uint32_t)(p1)<<24)|((uint32_t)(c1)<<26))
#define QSPI_SIZE 0x00800000u   /* 8 MiB W25Q64 */

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
 * Application routine placed in the FlexSPI AHB window.  `noinline`/`used` keep
 * it a real out-of-line call (executed in place), and the loop is genuine work
 * the compiler cannot fold into the caller: sum of squares 1..n.
 *
 * The iteration count is load-bearing.  QEMU *can* fetch instructions from an
 * MMIO region — it simply refuses to cache the translation block
 * (accel/tcg/translator.c: "If the second page is MMIO ... we do not cache the
 * TB"), so every instruction becomes a fresh translation and XIP runs ~100x
 * slower.  The failure signature is therefore a WALL CLOCK, not a crash: with a
 * ten-iteration loop this test would sail through a window that had regressed to
 * memory_region_init_io() and never notice.  Millions of iterations make the
 * difference impossible to miss, and run.sh enforces the time budget.
 * (Mechanism established by rt1180emulator, who measured 116x on the same class
 * of window; my own 10-iteration loop was blind to it.)
 */
__attribute__((section(".xip"), noinline, used))
static uint32_t xip_compute(uint32_t n)
{
    uint32_t acc = 0;
    for (uint32_t i = 1; i <= n; i++) {
        acc += i * i;
    }
    return acc;
}

void cpu0_main(void)
{
    int ok_coherent = 0;

    LP_CTRL = CTRL_TE;
    puts_("XIP test\r\n");

    /* The routine must really live in the QSPI window, not in flash/SRAM. */
    uint32_t fn = (uint32_t)(uintptr_t)&xip_compute;
    int in_xip = (fn >= QSPI_BASE) && (fn < QSPI_BASE + QSPI_SIZE);

    /* Indirect call -> BLX to the absolute QSPI address: runs in place. */
    uint32_t (*volatile fp)(uint32_t) = xip_compute;
    uint32_t r = fp(3000000);   /* sum of squares 1..3e6, mod 2^32 */

    /*
     * COHERENCE: the XIP window is only a MIRROR of the flash — m25p80 is the
     * authority.  QEMU's -kernel loader writes an XIP image straight into that
     * mirror, so unless the controller programs it into the NOR, the two
     * disagree: the mirror runs code the flash has never heard of, and the next
     * erase silently resurrects stale content underneath a running image.  That
     * would be a brand-new silent-wrong created by the fix for one.
     *
     * So ask the FLASH what is at the XIP function's address, over the IP command
     * path, and require it to match what the CPU is executing from the window.
     */
    {
        uint32_t off = fn & 0x00FFFFFFu;      /* flash offset of xip_compute */
        volatile uint32_t *win = (volatile uint32_t *)(fn & ~3u);
        int coherent = 1;

        /* LUT seq 0: READ (0x03) + 24-bit address. */
        FSPI_LUT(0) = LUT_SEQ(0x01, 0, 0x03, 0x02, 0, 24);
        FSPI_LUT(1) = LUT_SEQ(0x09, 0, 0x04, 0x00, 0, 0);

        FSPI_IPRXFCR = 1;                     /* clear RX FIFO */
        FSPI_INTR = 0x9;                      /* W1C DONE|ERR */
        FSPI_IPCR0 = off & ~3u;
        FSPI_IPCR1 = 16 | (0u << 16);         /* 16 bytes, seq 0 */
        FSPI_IPCMD = 1;
        while (!(FSPI_INTR & 0x1)) { }

        for (int i = 0; i < 4; i++) {
            if (FSPI_RFDR(i) != win[i]) {
                coherent = 0;                 /* flash and mirror disagree */
            }
        }
        ok_coherent = coherent;
    }

    puts_((in_xip && r == 0x9F726920u && ok_coherent)
          ? "XIP PASS\r\n" : "XIP FAIL\r\n");

    /* Exit so the harness can time the run: an XIP window that regressed to
     * MMIO still prints PASS, just ~100x later, and only a wall clock sees it.
     * AIRCR[SYSRESETREQ] with -no-reboot terminates QEMU. */
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0004u;
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[16] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
};
