/*
 * MCXN947 eIQ Neutron NPU model test.
 *
 * Two things must both hold, and the second is the one that matters:
 *
 *   1. The model never HANGS the guest.  The Neutron compute path is proprietary
 *      microcode, so we ack the exec/done handshake: a kick must read back idle
 *      so neutron_exec (while bit31) and neutron_done (while != 0) fall through.
 *
 *   2. The GUEST can tell the result is UNCOMPUTED.  This is the whole point.
 *      Acking DONE without computing, while writing nothing to the output buffer,
 *      hands firmware plausible-looking zeros it cannot distinguish from a real
 *      inference — a silent wrong answer.  It is not enough for the model to be
 *      honest on the HOST side (QMP compute-modelled=false, a LOG_UNIMP): the
 *      firmware under test cannot see any of that.  The truth has to reach the
 *      guest, and it does, through the non-gating INTR[ERRORTRAP] channel.
 *
 * The trap is deliberately NOT the completion gate: faulting an NXP accelerator
 * through its completion retcode hangs the driver instead of informing it.
 *
 * Prints "NEUTRON OK" only if the kicks retired without hanging AND the guest
 * saw the uncomputed trap.  jobs-started is cross-checked via QMP.
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

#define NEUTRON 0x400BE000u
#define CTRL (*(volatile int32_t *)(NEUTRON + 0x00))
#define INTR (*(volatile uint32_t *)(NEUTRON + 0x40))

#define INTR_INTREN      (1u << 0)
#define INTR_EVENTEN     (1u << 1)
#define INTR_ERRORTRAP_M (1u << 2)
#define INTR_ERRORTRAP_R (1u << 3)
#define INTR_ERRORTRAP   (INTR_ERRORTRAP_M | INTR_ERRORTRAP_R)

void cpu0_main(void)
{
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("NEUTRON test\r\n");

    INTR = INTR_EVENTEN;              /* EVENTEN, as neutronFwInit does */

    /* Before any kick, nothing is uncomputed yet. */
    ok &= !(INTR & INTR_ERRORTRAP);

    /* Mimic the eIQ microcode interpreter: many per-operator kicks. */
    for (int i = 0; i < 8; i++) {
        CTRL = 0x80000001;            /* kick (pipeline_enable; HW sets busy) */
        while (CTRL < 0) {            /* neutron_exec: wait SHADOW_BUSY clear */
            __asm__ volatile ("wfe");
        }
        while (CTRL != 0) {           /* neutron_done: wait fully idle */
            __asm__ volatile ("wfe");
        }
    }

    /*
     * The kicks retired without hanging (1), and the guest can SEE that the
     * inference was never actually computed (2).  Firmware that trusted the
     * DONE ack alone would be reading an output buffer we never wrote.
     */
    ok &= !!(INTR & INTR_ERRORTRAP);

    puts_(ok ? "NEUTRON OK\r\n" : "NEUTRON FAIL\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[16] = {
    [0] = (vec_t)0x20010000u,
    [1] = cpu0_main,
};
