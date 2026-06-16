/* MCXN947 eDMA test: programs DMA0 channel 0 to copy a 16-word buffer in SRAM
 * via a software-triggered (TCD_CSR.START) transfer, then verifies the copy and
 * the CH_CSR.DONE flag. SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdint.h>
#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t*)(LP+0x14))
#define LP_CTRL (*(volatile uint32_t*)(LP+0x18))
#define LP_DATA (*(volatile uint32_t*)(LP+0x1C))
static void puts_(const char*s){ LP_CTRL=(1u<<19); while(*s){ while(!(LP_STAT&(1u<<23))){} LP_DATA=(uint8_t)*s++; } }

#define CH0 0x40081000u    /* DMA0 channel 0 (base 0x40080000 + 0x1000) */
#define R32(o) (*(volatile uint32_t*)(CH0+(o)))
#define R16(o) (*(volatile uint16_t*)(CH0+(o)))

void cpu0_main(void)
{
    volatile uint32_t *src = (volatile uint32_t *)0x20001000u;
    volatile uint32_t *dst = (volatile uint32_t *)0x20002000u;
    int i, ok = 1;
    puts_("DMA test\r\n");
    for (i = 0; i < 16; i++) { src[i] = 0x1000u + i; dst[i] = 0; }

    R32(0x20) = 0x20001000u;   /* TCD_SADDR */
    R16(0x24) = 4;             /* TCD_SOFF  */
    R16(0x26) = 0x0202;        /* TCD_ATTR: SSIZE=DSIZE=2 (4-byte) */
    R32(0x28) = 64;            /* TCD_NBYTES = 16 words */
    R32(0x30) = 0x20002000u;   /* TCD_DADDR */
    R16(0x34) = 4;             /* TCD_DOFF  */
    R16(0x36) = 1;             /* TCD_CITER */
    R16(0x3E) = 1;             /* TCD_BITER */
    R16(0x3C) = 1;             /* TCD_CSR START -> run */

    for (i = 0; i < 16; i++) { if (dst[i] != 0x1000u + i) ok = 0; }
    if (!(R32(0x00) & 0x40000000u)) ok = 0;   /* CH_CSR.DONE */

    puts_(ok ? "DMA PASS\r\n" : "DMA FAIL\r\n");
    for (;;) {}
}
typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
