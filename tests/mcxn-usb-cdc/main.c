/*
 * MCXN947 USBHS (ChipIdea HS) CDC-ACM device gadget.
 *
 * A coherent USB CDC-ACM (Abstract Control Model) serial device on the ChipIdea
 * HS controller: interface 0 Communications/ACM + an EP2-IN interrupt
 * notification endpoint, interface 1 CDC-Data + EP1 bulk in/out (a byte echo).
 * With the usbredir core's gadget-profile=cdc, a real Linux host binds cdc_acm
 * and exposes /dev/ttyACMx.  Standalone it enumerates as CDC and echoes bulk
 * data on the data endpoints.  Prints "CDC ENUM OK" once SET_CONFIGURATION lands.
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
#define USBCMD 0x140
#define USBSTS 0x144
#define USBINTR 0x148
#define DEVICEADDR 0x154
#define ENDPTLISTADDR 0x158
#define USBMODE 0x1A8
#define ENDPTSETUPSTAT 0x1AC
#define ENDPTPRIME 0x1B0
#define ENDPTCOMPLETE 0x1BC
#define ENDPTCTRL0 0x1C0
#define ENDPTCTRL1 0x1C4
#define ENDPTCTRL2 0x1C8
#define CMD_RS (1u << 0)
#define STS_UI (1u << 0)
#define INTR_UE (1u << 0)
#define MODE_CM_DEVICE 0x2

#define DQH_BASE  0x20003000u
#define DTD(n)    (0x20004000u + (n) * 32u)
#define EP0IN_DTD   DTD(0)
#define EP0OUT_DTD  DTD(1)
#define EP1IN_DTD   DTD(2)
#define EP1OUT_DTD  DTD(3)
#define EP0IN_BUF   0x20005000u
#define EP1IN_BUF   0x20005400u
#define EP1OUT_BUF  0x20005800u
#define EP0OUT_BUF  0x20005C00u   /* scratch for control-OUT data (SET_LINE_CODING) */
#define EP1_CAP     512

#define DQH(ep, in)  (DQH_BASE + ((ep) * 2 + (in)) * 64u)
#define M(a)  (*(volatile uint32_t *)(uintptr_t)(a))

/* CDC-ACM device (bDeviceClass=Communications), VID 1FC9 / PID 0095. */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
    0xC9, 0x1F, 0x95, 0x00, 0x00, 0x01, 0, 0, 0, 1,
};
static const uint8_t cfg_desc[67] = {
    9, 2, 67, 0, 2, 1, 0, 0x80, 50,               /* configuration, 2 ifaces */
    9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,           /* iface0 Comm/ACM, 1 ep */
    5, 0x24, 0x00, 0x10, 0x01,                    /* CDC header, bcdCDC=1.10 */
    5, 0x24, 0x01, 0x00, 1,                       /* CDC call mgmt, data iface=1 */
    4, 0x24, 0x02, 0x02,                          /* CDC ACM, caps=line+ctrl */
    5, 0x24, 0x06, 0, 1,                          /* CDC union, master0 slave1 */
    7, 5, 0x82, 0x03, 16, 0, 9,                   /* EP2 IN interrupt, notify */
    9, 4, 1, 0, 2, 0x0A, 0, 0, 0,                 /* iface1 CDC-Data, 2 eps */
    7, 5, 0x01, 0x02, 0x00, 0x02, 0,              /* EP1 OUT bulk 512 */
    7, 5, 0x81, 0x02, 0x00, 0x02, 0,              /* EP1 IN  bulk 512 */
};
static const uint8_t devqual_desc[10] = {
    10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0,
};
/* CDC line coding: 115200 8N1 (dwDTERate, stop, parity, data bits). */
static const uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };

static uint8_t setup[8];
static volatile int enum_done;
static volatile int ep0_out_status;   /* a control-OUT data stage is pending its status */

static void prime(int ep, int in, uint32_t dtd, uint32_t buf, int len)
{
    M(dtd + 0)  = 1;
    M(dtd + 4)  = (1u << 7) | (1u << 15) | ((uint32_t)len << 16);
    M(dtd + 8)  = buf;
    M(dtd + 12) = 0; M(dtd + 16) = 0; M(dtd + 20) = 0; M(dtd + 24) = 0;
    M(DQH(ep, in) + 8) = dtd;
    R(ENDPTPRIME) = in ? (1u << (16 + ep)) : (1u << ep);
}

static void ep0_send(const uint8_t *data, int len, int wlen)
{
    if (len > wlen) { len = wlen; }
    if (len > 256)  { len = 256; }        /* the 67-byte config needs > one packet */
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

    if (bmreq == 0x80 && breq == 6) {              /* GET_DESCRIPTOR */
        uint8_t type = wval >> 8;
        if (type == 1)      { ep0_send(dev_desc, 18, wlen); }
        else if (type == 2) { ep0_send(cfg_desc, 67, wlen); }
        else if (type == 6) { ep0_send(devqual_desc, 10, wlen); }
        else                { ep0_send(0, 0, 0); }
    } else if (bmreq == 0x00 && breq == 5) {       /* SET_ADDRESS */
        R(DEVICEADDR) = ((uint32_t)(wval & 0x7F) << 25) | (1u << 24);
        ep0_send(0, 0, 0);
    } else if (bmreq == 0x00 && breq == 9) {       /* SET_CONFIGURATION */
        /* Arm EP1 bulk OUT BEFORE the status stage: the status-IN is what makes
         * the host see the config as done, so arming the data EP first closes
         * the window where the host could write into an un-armed EP1-OUT. */
        prime(1, 0, EP1OUT_DTD, EP1OUT_BUF, EP1_CAP);   /* arm EP1 bulk OUT first */
        ep0_send(0, 0, 0);                              /* then status stage */
        enum_done = 1;
    } else if (bmreq == 0xA1 && breq == 0x21) {    /* CDC GET_LINE_CODING */
        ep0_send(line_coding, 7, wlen);
    } else if (bmreq == 0x21 && breq == 0x22) {    /* CDC SET_CONTROL_LINE_STATE */
        ep0_send(0, 0, 0);
    } else if (bmreq == 0x21 && breq == 0x20) {    /* CDC SET_LINE_CODING (7B OUT) */
        /* Control-OUT with a data stage: arm EP0 OUT to receive the 7 bytes;
         * the status IN is sent once they arrive (see the ISR). */
        prime(0, 0, EP0OUT_DTD, EP0OUT_BUF, wlen ? wlen : 7);
        ep0_out_status = 1;
    } else if ((bmreq & 0x80) && breq == 0) {      /* GET_STATUS */
        static const uint8_t st[2] = { 0, 0 };
        ep0_send(st, 2, wlen);
    } else if (bmreq & 0x80) {                     /* other IN requests: short */
        ep0_send(0, 0, 0);
    } else if (wlen) {                             /* other no-model OUT w/ data */
        prime(0, 0, EP0OUT_DTD, EP0OUT_BUF, wlen);
        ep0_out_status = 1;
    } else {
        ep0_send(0, 0, 0);                         /* no-data OUT: status IN */
    }
}

void usb_isr(void)
{
    uint32_t sts = R(USBSTS);

    if (!(sts & STS_UI)) { return; }
    R(USBSTS) = STS_UI;

    if (R(ENDPTSETUPSTAT) & 1) {
        setup[0] = M(DQH(0, 0) + 0x28); setup[1] = M(DQH(0, 0) + 0x28) >> 8;
        setup[2] = M(DQH(0, 0) + 0x28) >> 16; setup[3] = M(DQH(0, 0) + 0x28) >> 24;
        setup[4] = M(DQH(0, 0) + 0x2C); setup[5] = M(DQH(0, 0) + 0x2C) >> 8;
        setup[6] = M(DQH(0, 0) + 0x2C) >> 16; setup[7] = M(DQH(0, 0) + 0x2C) >> 24;
        R(ENDPTSETUPSTAT) = 1;
        handle_setup();
    }

    uint32_t cmpl = R(ENDPTCOMPLETE);
    if (cmpl) {
        R(ENDPTCOMPLETE) = cmpl;
        if ((cmpl & (1u << 0)) && ep0_out_status) { /* EP0 OUT data received */
            ep0_out_status = 0;
            ep0_send(0, 0, 0);                      /* zero-length status IN */
        }
        if (cmpl & (1u << 1)) {                    /* EP1 OUT complete -> echo */
            uint32_t tok = M(EP1OUT_DTD + 4);
            int recv = EP1_CAP - ((tok >> 16) & 0x7FFF);
            for (int i = 0; i < recv && i < EP1_CAP; i++) {
                ((volatile uint8_t *)(uintptr_t)EP1IN_BUF)[i] =
                    ((volatile uint8_t *)(uintptr_t)EP1OUT_BUF)[i];
            }
            prime(1, 1, EP1IN_DTD, EP1IN_BUF, recv);
            prime(1, 0, EP1OUT_DTD, EP1OUT_BUF, EP1_CAP);
        }
    }
}

static void dqh_init(int ep, int in, int mps, int ios)
{
    M(DQH(ep, in) + 0x00) = ((uint32_t)mps << 16) | (ios ? (1u << 15) : 0);
    M(DQH(ep, in) + 0x04) = 0;
    M(DQH(ep, in) + 0x08) = 1;
    M(DQH(ep, in) + 0x0C) = 0;
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("USBHS CDC-ACM test\r\n");

    R(USBMODE) = MODE_CM_DEVICE;
    R(ENDPTLISTADDR) = DQH_BASE;

    dqh_init(0, 0, 64, 1);                  /* EP0 OUT (IOS) */
    dqh_init(0, 1, 64, 0);                  /* EP0 IN */
    dqh_init(1, 0, 512, 0);                 /* EP1 OUT data bulk */
    dqh_init(1, 1, 512, 0);                 /* EP1 IN  data bulk */
    dqh_init(2, 1, 16, 0);                  /* EP2 IN interrupt (notify) */
    R(ENDPTCTRL0) = (1u << 23) | (1u << 7);                          /* EP0 control */
    R(ENDPTCTRL1) = (1u << 23) | (2u << 17) | (1u << 7) | (2u << 2); /* EP1 bulk */
    R(ENDPTCTRL2) = (1u << 23) | (3u << 17);                         /* EP2 IN interrupt */

    R(USBINTR) = INTR_UE;
    *(volatile uint32_t *)0xE000E108u = (1u << (67 - 64));  /* NVIC ISER2: IRQ 67 */
    __asm__ volatile ("cpsie i");
    R(USBCMD) = CMD_RS;

    while (!enum_done) {}
    puts_("CDC ENUM OK\r\n");
    for (;;) {}
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[96] = {
    [0]  = (vec_t)0x20010000u,
    [1]  = cpu0_main,
    [16 + 67] = usb_isr,
};
