/*
 * MCXN947 SMARTDMA register-fidelity test (the EZH datapath is NOT modelled —
 * proprietary microcode, no ISA in the RM, no reference impl — so this proves
 * the model is HONEST and INFORMATIVE, never silently wrong).
 *
 * Three things, all guest- or log-observable:
 *
 *   1. HONEST-FAULT (guest-visible): a keyed BOOT (CTRL = 0xC0DE0011) does NOT
 *      run the program, so the engine never completes and CTRL.START (bit0)
 *      reads back STILL SET.  A model that self-cleared START would tell a
 *      polling guest "done" over an untouched buffer — a silent wrong answer.
 *
 *   2. INFORMATIVE (log-visible): the model decodes WHICH documented op was
 *      requested — it recovers the apiIndex from the firmware jump table the
 *      SDK installs at SRAMX 0x0400_0000 (s_smartdmaApiTable[apiIndex] resolved
 *      into BOOTADR by SMARTDMA_Boot()).  Here apiIndex 4 = "RGB565To888".
 *
 *   3. KEYED CTRL (log- + behaviour-visible): CTRL is keyed with 0xC0DE in the
 *      high half.  A keyless START-looking write (0x0000_0011) is NOT a valid
 *      command and must be REJECTED, not booted (so exactly ONE boot happens).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE (1u << 19)
#define STAT_TDRE (1u << 23)
static void putc_(char c){ while(!(LP_STAT&STAT_TDRE)){} LP_DATA=(uint8_t)c; }
static void puts_(const char*s){ while(*s) putc_(*s++); }

/* SMARTDMA @ 0x4003_3000 (CMSIS SMARTDMA_Type). */
#define SDMA          0x40033000u
#define SDMA_BOOTADR  (*(volatile uint32_t *)(SDMA + 0x20))
#define SDMA_CTRL     (*(volatile uint32_t *)(SDMA + 0x24))
#define SDMA_ARM2EZH  (*(volatile uint32_t *)(SDMA + 0x40))

/* SDK firmware install address (SMARTDMA_DISPLAY_MEM_ADDR = SRAMX). */
#define FW_BASE   0x04000000u
#define MEM32(a)  (*(volatile uint32_t *)(uintptr_t)(a))

void cpu0_main(void)
{
    int ok = 1;
    int i;

    LP_CTRL = CTRL_TE;
    puts_("SMARTDMA test\r\n");

    /*
     * Install a display-firmware jump table into SRAMX.  entry[0] is the
     * display fingerprint the model recognises (matches the real
     * s_smartdmaDisplayFirmware[0..3] = 0x0400_0024); entry[4] is the
     * RGB565To888 function pointer (apiIndex 4).  Values are distinct so the
     * model's scan resolves BOOTADR to exactly one slot.
     */
    for (i = 0; i < 9; i++) {
        MEM32(FW_BASE + i * 4) = 0x04000100u + (uint32_t)i * 0x40u;
    }
    MEM32(FW_BASE + 0 * 4) = 0x04000024u;   /* display fingerprint */
    MEM32(FW_BASE + 4 * 4) = 0x04000ABCu;   /* apiIndex 4 = RGB565To888 */

    /* Keyed init (GPISYNCH, no boot): START must be CLEAR — a boot has not run. */
    SDMA_CTRL = 0xC0DE0010u;
    ok &= !(SDMA_CTRL & 1u);
    puts_((SDMA_CTRL & 1u) ? "INIT-START-SET\r\n" : "INIT-NO-START\r\n");

    /* Boot apiIndex 4: ARM2EZH carries pParam|mask, BOOTADR = entry[4]. */
    SDMA_ARM2EZH = 0x20002000u | 2u;        /* pParam=0x20002000, mask=2 */
    SDMA_BOOTADR = 0x04000ABCu;
    SDMA_CTRL    = 0xC0DE0011u;              /* keyed BOOT */

    /* Honest-fault: the engine never completes, so START reads back STILL SET. */
    ok &= (SDMA_CTRL & 1u);
    puts_((SDMA_CTRL & 1u) ? "START-STILL-SET\r\n" : "START-CLEARED\r\n");

    /* Keyless START-looking write: NOT a valid command — must be rejected, not
     * booted (the log must still show exactly ONE boot). */
    SDMA_CTRL = 0x00000011u;

    puts_(ok ? "SMARTDMA OK\r\n" : "SMARTDMA FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[16] = {
    [0] = (vec_t)0x20010000u,
    [1] = cpu0_main,
};
