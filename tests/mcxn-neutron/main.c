/*
 * MCXN947 eIQ Neutron NPU model test — mimics the eIQ driver's exec/done
 * handshake to prove the model never hangs the guest (the proprietary compute
 * is acked, not run).  CTRL @ 0x400B_E000+0x00: a kick must read back idle so
 * neutron_exec (while bit31) and neutron_done (while !=0) both fall through.
 * Prints "NEUTRON OK" after several kicks; jobs-started is checked via QMP.
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

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("NEUTRON test\r\n");

    INTR = 0x2;                       /* EVENTEN, as neutronFwInit does */

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

    puts_("NEUTRON OK\r\n");          /* reached only if no kick hung */
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[16] = {
    [0] = (vec_t)0x20010000u,
    [1] = cpu0_main,
};
