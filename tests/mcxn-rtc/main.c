/*
 * MCXN947 RTC (calendar) live-tick + alarm + NVIC interrupt test.
 *
 * Unlocks the RTC, sets the calendar to 00:00:58 (day 1, month 1) and an alarm
 * at 00:01:00, enables the 1 Hz and alarm interrupts (IER) and the RTC NVIC
 * line (IRQ 52).  The model's free-running 1 Hz tick advances the calendar:
 * within two ticks SECONDS rolls 58 -> 59 -> 00 (carry to minute 1), the 1 Hz
 * status flag is delivered each second, and the calendar matches the alarm.
 * The ISR counts the 1 Hz events and the alarm.  Prints "RTC PASS" once a 1 Hz
 * tick and the alarm have both fired and the calendar shows the rollover -
 * exercising the live tick -> ISR -> NVIC -> handler path.
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

#define RTC 0x4004C000u
#define R16(o) (*(volatile uint16_t *)(RTC + (o)))
#define YEARMON     R16(0x00)
#define DAYS        R16(0x02)
#define HOURMIN     R16(0x04)
#define SECONDS     R16(0x06)
#define ALM_YEARMON R16(0x08)
#define ALM_DAYS    R16(0x0A)
#define ALM_HOURMIN R16(0x0C)
#define ALM_SECONDS R16(0x0E)
#define STATUS      R16(0x12)
#define ISR         R16(0x14)
#define IER         R16(0x16)

#define STATUS_WE   0x00C0u   /* write-enable field = 0b11 */
#define ISR_ALM     0x0004u
#define ISR_1HZ     0x0040u
#define IER_ALM     0x0004u
#define IER_1HZ     0x0040u

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define RTC_IRQ 52

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

static volatile uint32_t hz_count;
static volatile uint32_t alm_fired;

void rtc_handler(void)
{
    uint16_t flags = ISR;
    if (flags & ISR_1HZ) {
        hz_count++;
    }
    if (flags & ISR_ALM) {
        alm_fired = 1;
    }
    ISR = flags & (ISR_1HZ | ISR_ALM);   /* write-1-to-clear */
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("RTC test\r\n");

    STATUS = STATUS_WE;       /* unlock calendar for writes */

    YEARMON = 0x0001;         /* month 1 */
    DAYS    = 0x0001;         /* day 1 */
    HOURMIN = 0x0000;         /* 00:00 */
    SECONDS = 58;

    ALM_YEARMON = 0x0001;
    ALM_DAYS    = 0x0001;
    ALM_HOURMIN = 0x0001;     /* minute 1, hour 0 */
    ALM_SECONDS = 0x0000;     /* second 0 -> alarm at 00:01:00 */

    ISR = ISR_1HZ | ISR_ALM;  /* clear stale flags */
    IER = IER_1HZ | IER_ALM;
    NVIC_ISER1 = (1u << (RTC_IRQ - 32));
    __asm__ volatile ("cpsie i");

    while (!alm_fired || hz_count < 1) {
    }

    if ((SECONDS & 0x3F) == 0 && (HOURMIN & 0x3F) == 1) {
        puts_("RTC PASS\r\n");
    } else {
        puts_("RTC FAIL\r\n");
    }
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + RTC_IRQ] = rtc_handler,  /* exception 68 = IRQ 52 */
};
