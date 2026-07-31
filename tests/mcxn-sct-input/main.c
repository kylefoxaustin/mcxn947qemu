/*
 * SCT INPUT-CONDITIONED EVENTS -- an SCT input pin edge fires an SCT event.
 *
 *     SCT input pin (AINn, selected by INPUTMUX SCT0_INMUX)  ->  event whose
 *     EV[n].CTRL selects COMBMODE=IO / IOSEL=n / IOCOND=edge  ->  EVFLAG[n] (+ IRQ).
 *
 * This is how the SCT reacts to an external signal (capture/abort/state-change on a pin).
 * It was absent: only the match/limit event 0 (the counter path) existed, so an SCT
 * configured to act on an input pin did nothing.  The input pin has no signal source in
 * emulation, so it is OPERATOR-DRIVEN over QMP ("sct-inputs" QOM property).
 *
 * Two events are armed to prove the selection is real:
 *   EV1: COMBMODE=IO, IOSEL=input0, IOCOND=RISE
 *   EV2: COMBMODE=IO, IOSEL=input3, IOCOND=RISE
 * and the operator drives ONLY input0:
 *   PHASE A (input0 0->1): EV1 fires (EVFLAG bit1) but EV2 does NOT (IOSEL routing).
 *   PHASE B (input0 1->0): EV1 must NOT fire -- a falling edge with IOCOND=RISE (edge gate).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LPUART4 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4 + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4 + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4 + 0x1C))
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

/* ---- SCT0 (base 0x40091000) ---------------------------------------------- */
#define SCT0 0x40091000u
#define SCT_EVFLAG    (*(volatile uint32_t *)(SCT0 + 0x0F4))
#define SCT_EV1_STATE (*(volatile uint32_t *)(SCT0 + 0x300 + 1 * 8))
#define SCT_EV1_CTRL  (*(volatile uint32_t *)(SCT0 + 0x304 + 1 * 8))
#define SCT_EV2_STATE (*(volatile uint32_t *)(SCT0 + 0x300 + 2 * 8))
#define SCT_EV2_CTRL  (*(volatile uint32_t *)(SCT0 + 0x304 + 2 * 8))
#define EVFLAG_EV1 (1u << 1)
#define EVFLAG_EV2 (1u << 2)

/* EV[n].CTRL: COMBMODE(13:12)=2 IO, IOCOND(11:10)=1 RISE, IOSEL(9:6), OUTSEL(5)=0 input. */
#define EV_IO_RISE(iosel) ((2u << 12) | (1u << 10) | ((iosel) << 6))

static void spin(int n)
{
    volatile int d;

    for (d = 0; d < n; d++) {
    }
}

void cpu0_main(void)
{
    int ok = 1;
    volatile int d;

    LP_CTRL = CTRL_TE;
    puts_("SCT-INPUT test\r\n");

    /* Arm two input-conditioned events, enabled in state 0 (STATE mask bit 0). */
    SCT_EV1_CTRL  = EV_IO_RISE(0);       /* input 0, rising */
    SCT_EV1_STATE = 1;
    SCT_EV2_CTRL  = EV_IO_RISE(3);       /* input 3, rising */
    SCT_EV2_STATE = 1;

    /* PHASE A -- the operator drives input 0 HIGH (a rising edge). */
    puts_("SCT-INPUT ARMED\r\n");
    for (d = 0; d < 80000000 && !(SCT_EVFLAG & EVFLAG_EV1); d++) {
    }
    puts_("  phaseA EVFLAG="); puthex(SCT_EVFLAG); puts_("\r\n");
    ok &= !!(SCT_EVFLAG & EVFLAG_EV1);   /* input-0 rising fired EV1 */
    ok &= !(SCT_EVFLAG & EVFLAG_EV2);    /* input-3 event did NOT fire (IOSEL routing) */
    puts_(ok ? "  phaseA: input0 rising fired EV1 only (correct)\r\n"
             : "  phaseA: FAILED\r\n");

    /* PHASE B -- clear, then the operator drives input 0 LOW (a falling edge).  With
     * IOCOND=RISE, EV1 must NOT fire. */
    SCT_EVFLAG = EVFLAG_EV1 | EVFLAG_EV2;   /* W1C */
    puts_("SCT-INPUT FALLING\r\n");
    spin(20000000);                      /* operator injects the falling edge here */
    puts_("  phaseB EVFLAG="); puthex(SCT_EVFLAG); puts_("\r\n");
    if (SCT_EVFLAG & EVFLAG_EV1) {
        puts_("  FAIL: falling edge fired a RISE-conditioned event\r\n");
        ok = 0;
    } else {
        puts_("  phaseB: falling edge ignored (IOCOND correct)\r\n");
    }

    puts_(ok ? "SCT-INPUT PASS\r\n" : "SCT-INPUT FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
