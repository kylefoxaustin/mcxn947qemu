/* MCXN947 OSTIMER match-interrupt test: arms a gray-coded match ahead of the
 * current counter, enables the OS-event IRQ, and waits for the ISR.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t*)(LP+0x14))
#define LP_CTRL (*(volatile uint32_t*)(LP+0x18))
#define LP_DATA (*(volatile uint32_t*)(LP+0x1C))
static void puts_(const char*s){ LP_CTRL=(1u<<19); while(*s){ while(!(LP_STAT&(1u<<23))){} LP_DATA=(uint8_t)*s++; } }

#define OST 0x40049000u
#define EVTL   (*(volatile uint32_t*)(OST+0x00))
#define MATL   (*(volatile uint32_t*)(OST+0x10))
#define MATH   (*(volatile uint32_t*)(OST+0x14))
#define OSCTRL (*(volatile uint32_t*)(OST+0x1C))
#define NVIC_ISER(n) (*(volatile uint32_t*)(0xE000E100u + 4*(n)))
#define OSEVENT_IRQ 57

static uint32_t g2b(uint32_t g){ uint32_t b=g; while(g>>=1) b^=g; return b; }
static uint32_t b2g(uint32_t n){ return n ^ (n>>1); }

static volatile int fired;
void osevent_handler(void){ OSCTRL = OSCTRL | 1u; /* W1C INTRFLAG */ fired=1; }

void cpu0_main(void)
{
    puts_("OSTIMER test\r\n");
    uint32_t cur = g2b(EVTL);          /* current binary counter (low 32) */
    uint32_t g = b2g(cur + 50000u);    /* match ~50ms ahead at 1 MHz */
    MATL = g; MATH = 0;
    NVIC_ISER(OSEVENT_IRQ/32) = (1u << (OSEVENT_IRQ%32));
    __asm__ volatile("cpsie i");
    OSCTRL = 2u;                       /* INTENA */
    while (!fired) {}
    puts_("OSTIMER PASS\r\n");
    for(;;){}
}
typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]=(vec_t)0x20010000u, [1]=cpu0_main, [16+OSEVENT_IRQ]=osevent_handler,
};
