/*
 * MCXN947 USBDCD — USB Device Charger Detection, the BC1.2 sequence end to end.
 *
 * Runs the detection the way a stock charger-detect driver does: enable BC1.2, START,
 * then walk STATUS[SEQ_STAT]/[SEQ_RES] one interrupt-flag (CONTROL.IF) per phase,
 * acknowledging each with CONTROL.IACK, until the sequence is no longer ACTIVE.  Prints
 * the classification it arrived at.
 *
 * The port is operator-driven: -global mcxn-usbdcd.charger=0|1|2|3 (none/SDP/CDP/DCP).
 * With no charger the honest result is a contact-detect TIMEOUT, not a fabricated port.
 * The run.sh sweeps all four and asserts each classification -- so a model that stamped
 * one fixed answer (the old register-only behaviour) fails every case but the one it faked.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP_CTRL (*(volatile uint32_t *)0x400B4018u)
#define LP_STAT (*(volatile uint32_t *)0x400B4014u)
#define LP_DATA (*(volatile uint32_t *)0x400B401Cu)
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }

#define DCD      0x400DC000u
#define CONTROL  (*(volatile uint32_t *)(DCD + 0x00))
#define STATUS   (*(volatile uint32_t *)(DCD + 0x08))

#define C_IACK   (1u << 0)
#define C_IF     (1u << 8)
#define C_IE     (1u << 16)
#define C_BC12   (1u << 17)
#define C_START  (1u << 24)

#define S_SEQ_RES(s)  (((s) >> 16) & 3u)
#define S_SEQ_STAT(s) (((s) >> 18) & 3u)
#define S_TO          (1u << 21)
#define S_ACTIVE      (1u << 22)

void cpu0_main(void)
{
    uint32_t st = 0;
    int guard;

    LP_CTRL = (1u << 19);
    puts_("USBDCD test\r\n");

    CONTROL = C_IE | C_BC12;                 /* enable interrupt + BC1.2 mode */
    CONTROL = C_IE | C_BC12 | C_START;       /* begin the sequence */

    /* Walk the phases: one CONTROL.IF per phase, IACK advances to the next. */
    for (guard = 0; guard < 16; guard++) {
        int g2 = 2000000;

        while (!(CONTROL & C_IF) && g2--) {
        }
        st = STATUS;
        CONTROL = C_IE | C_BC12 | C_IACK;    /* ack this phase; HW advances if ACTIVE */
        if (!(st & S_ACTIVE)) {
            break;                           /* sequence complete (or timed out) */
        }
    }

    puts_("DCD RESULT: ");
    if (st & S_TO) {
        puts_("NONE\r\n");                   /* nothing attached -> contact timeout */
    } else if (S_SEQ_RES(st) == 1u) {
        puts_("SDP\r\n");
    } else if (S_SEQ_RES(st) == 2u && S_SEQ_STAT(st) == 3u) {
        puts_("CDP\r\n");
    } else if (S_SEQ_RES(st) == 3u) {
        puts_("DCP\r\n");
    } else {
        puts_("UNKNOWN\r\n");
    }

    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
