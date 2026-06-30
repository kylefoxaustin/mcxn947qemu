/*
 * MCXN947 USBHS (ChipIdea HS) device-mode enumeration + bulk test.
 *
 * Bare-metal high-speed USB device stack exercising the model's ChipIdea
 * device engine end-to-end.  It programs device Queue Heads (dQH) and Transfer
 * Descriptors (dTD), enables the controller in device mode (which makes the
 * model announce the device to the remote usbredir host), and services SETUP +
 * transfer-complete events in the USB1_HS ISR: GET_DESCRIPTOR (device+config),
 * SET_ADDRESS, SET_CONFIGURATION over EP0, then an EP1 bulk OUT->IN echo.
 *
 * Prints "USB ENUM OK" once SET_CONFIGURATION lands.  The same usbredir host as
 * the USBFS test drives it (the wire protocol is controller-agnostic).
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

static void putc_(char c) { while (!(LP_STAT & STAT_TDRE)) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) { putc_(*s++); } }

/* ---- USBHS (ChipIdea) registers ----------------------------------------- */
#define USBHS 0x4010B000u
#define R(o)  (*(volatile uint32_t *)(USBHS + (o)))
#define USBCMD        0x140
#define USBSTS        0x144
#define USBINTR       0x148
#define DEVICEADDR    0x154
#define ENDPTLISTADDR 0x158
#define USBMODE       0x1A8
#define ENDPTSETUPSTAT 0x1AC
#define ENDPTPRIME    0x1B0
#define ENDPTCOMPLETE 0x1BC
#define ENDPTCTRL0    0x1C0
#define ENDPTCTRL1    0x1C4

#define CMD_RS        (1u << 0)
#define STS_UI        (1u << 0)
#define INTR_UE       (1u << 0)
#define MODE_CM_DEVICE 0x2

/* Memory pools in SRAM (dQH list 2 KiB-aligned; dTDs 32 B-aligned). */
#define DQH_BASE  0x20003000u           /* 16 dQH * 64 B */
#define DTD(n)    (0x20004000u + (n) * 32u)
#define EP0IN_DTD   DTD(0)
#define EP0OUT_DTD  DTD(1)
#define EP1IN_DTD   DTD(2)
#define EP1OUT_DTD  DTD(3)
#define EP0IN_BUF   0x20005000u
#define EP1IN_BUF   0x20005100u
#define EP1OUT_BUF  0x20005200u
#define EP1_CAP     64

#define DQH(ep, in)  (DQH_BASE + ((ep) * 2 + (in)) * 64u)
#define M(a)  (*(volatile uint32_t *)(uintptr_t)(a))

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0xC9, 0x1F, 0x94, 0x00, 0x00, 0x01, 0, 0, 0, 1,
};
static const uint8_t cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,
    7, 5, 0x01, 2, 0x00, 0x02, 0,            /* EP1 OUT bulk, wMaxPacketSize=512 (HS) */
    7, 5, 0x81, 2, 0x00, 0x02, 0,            /* EP1 IN  bulk, wMaxPacketSize=512 (HS) */
};
/* USB 2.0 device_qualifier (HS-mandatory): the kernel demands this on a HS
 * device; a 0-length ack makes it retry + fail to finalize. */
static const uint8_t devqual_desc[10] = {
    10, 6, 0x00, 0x02, 0, 0, 0, 64, 1, 0,
};

static uint8_t setup[8];
static volatile int enum_done;

/* Prime a dTD on (ep,dir) with @len bytes at @buf, then ring ENDPTPRIME. */
static void prime(int ep, int in, uint32_t dtd, uint32_t buf, int len)
{
    M(dtd + 0)  = 1;                        /* nextDtd = terminate */
    M(dtd + 4)  = (1u << 7) | (1u << 15) | ((uint32_t)len << 16);  /* ACTIVE|IOC|bytes */
    M(dtd + 8)  = buf;                      /* buffer page 0 */
    M(dtd + 12) = 0; M(dtd + 16) = 0; M(dtd + 20) = 0; M(dtd + 24) = 0;
    M(DQH(ep, in) + 8) = dtd;               /* link dTD into the dQH */
    R(ENDPTPRIME) = in ? (1u << (16 + ep)) : (1u << ep);
}

static void ep0_send(const uint8_t *data, int len, int wlen)
{
    if (len > wlen) { len = wlen; }
    if (len > 64)   { len = 64; }
    for (int i = 0; i < len; i++) {
        ((volatile uint8_t *)(uintptr_t)EP0IN_BUF)[i] = data[i];
    }
    prime(0, 1, EP0IN_DTD, EP0IN_BUF, len);
}

static void handle_setup(void)
{
    uint8_t bmreq = setup[0], breq = setup[1];
    uint16_t wval = setup[2] | (setup[3] << 8);
    uint16_t wlen = setup[6] | (setup[7] << 8);

    if (bmreq == 0x80 && breq == 6) {           /* GET_DESCRIPTOR */
        uint8_t type = wval >> 8;
        if (type == 1) { puts_("  GET_DESC device\r\n"); ep0_send(dev_desc, 18, wlen); }
        else if (type == 2) { puts_("  GET_DESC config\r\n"); ep0_send(cfg_desc, 32, wlen); }
        else if (type == 6) { puts_("  GET_DESC qualifier\r\n"); ep0_send(devqual_desc, 10, wlen); }
        else { ep0_send(0, 0, 0); }
    } else if (bmreq == 0x00 && breq == 5) {    /* SET_ADDRESS */
        puts_("  SET_ADDRESS\r\n");
        R(DEVICEADDR) = ((uint32_t)(wval & 0x7F) << 25) | (1u << 24);  /* USBADRA */
        ep0_send(0, 0, 0);                      /* zero-length status IN */
    } else if (bmreq == 0x00 && breq == 9) {    /* SET_CONFIGURATION */
        puts_("  SET_CONFIG\r\n");
        ep0_send(0, 0, 0);
        prime(1, 0, EP1OUT_DTD, EP1OUT_BUF, EP1_CAP);   /* arm EP1 bulk OUT */
        enum_done = 1;
    } else if ((bmreq & 0x80) && breq == 0) {   /* GET_STATUS -> 2 bytes */
        static const uint8_t st[2] = { 0, 0 };
        ep0_send(st, 2, wlen);
    } else {
        ep0_send(0, 0, 0);
    }
}

void usb_isr(void)
{
    uint32_t sts = R(USBSTS);

    if (!(sts & STS_UI)) { return; }
    R(USBSTS) = STS_UI;                         /* W1C */

    if (R(ENDPTSETUPSTAT) & 1) {
        setup[0] = M(DQH(0, 0) + 0x28); setup[1] = M(DQH(0, 0) + 0x28) >> 8;
        setup[2] = M(DQH(0, 0) + 0x28) >> 16; setup[3] = M(DQH(0, 0) + 0x28) >> 24;
        setup[4] = M(DQH(0, 0) + 0x2C); setup[5] = M(DQH(0, 0) + 0x2C) >> 8;
        setup[6] = M(DQH(0, 0) + 0x2C) >> 16; setup[7] = M(DQH(0, 0) + 0x2C) >> 24;
        R(ENDPTSETUPSTAT) = 1;                  /* W1C */
        handle_setup();
    }

    uint32_t cmpl = R(ENDPTCOMPLETE);
    if (cmpl) {
        R(ENDPTCOMPLETE) = cmpl;                /* W1C */
        if (cmpl & (1u << 1)) {                 /* EP1 OUT complete -> echo */
            uint32_t tok = M(EP1OUT_DTD + 4);
            int recv = EP1_CAP - ((tok >> 16) & 0x7FFF);
            for (int i = 0; i < recv && i < EP1_CAP; i++) {
                ((volatile uint8_t *)(uintptr_t)EP1IN_BUF)[i] =
                    ((volatile uint8_t *)(uintptr_t)EP1OUT_BUF)[i];
            }
            prime(1, 1, EP1IN_DTD, EP1IN_BUF, recv);     /* echo on EP1 IN */
            prime(1, 0, EP1OUT_DTD, EP1OUT_BUF, EP1_CAP);/* re-arm EP1 OUT */
        }
    }
}

static void dqh_init(int ep, int in, int mps, int ios)
{
    M(DQH(ep, in) + 0x00) = ((uint32_t)mps << 16) | (ios ? (1u << 15) : 0);
    M(DQH(ep, in) + 0x04) = 0;
    M(DQH(ep, in) + 0x08) = 1;              /* nextDtd = terminate */
    M(DQH(ep, in) + 0x0C) = 0;
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("USBHS enum test\r\n");

    R(USBMODE) = MODE_CM_DEVICE;            /* device controller mode */
    R(ENDPTLISTADDR) = DQH_BASE;

    dqh_init(0, 0, 64, 1);                  /* EP0 OUT (IOS) */
    dqh_init(0, 1, 64, 0);                  /* EP0 IN */
    dqh_init(1, 0, 512, 0);                 /* EP1 OUT bulk */
    dqh_init(1, 1, 512, 0);                 /* EP1 IN bulk */
    R(ENDPTCTRL0) = (1u << 23) | (1u << 7); /* EP0 TXE+RXE (control) */
    R(ENDPTCTRL1) = (1u << 23) | (2u << 17) | (1u << 7) | (2u << 2);  /* EP1 bulk */

    R(USBINTR) = INTR_UE;
    *(volatile uint32_t *)0xE000E108u = (1u << (67 - 64));  /* NVIC ISER2: IRQ 67 */
    __asm__ volatile ("cpsie i");
    R(USBCMD) = CMD_RS;                     /* run: device appears on the bus */

    while (!enum_done) {}
    puts_("USB ENUM OK\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[96] = {
    [0]  = (vec_t)0x20010000u,   /* initial MSP */
    [1]  = cpu0_main,            /* Reset_Handler */
    [16 + 67] = usb_isr,         /* exception 83 = IRQ 67 (USB1_HS) */
};
