/*
 * MCXN947 USB device-mode enumeration test (USBFS / KHCI).
 *
 * Bare-metal USB device stack exercising the model's device-mode endpoint
 * engine end-to-end: it arms EP0 Buffer Descriptors, enables the controller
 * (which makes the model announce the device to the remote usbredir host), and
 * services SETUP tokens in the USB0_FS ISR — GET_DESCRIPTOR (device + config),
 * SET_ADDRESS, SET_CONFIGURATION — replying with descriptors via EP0 IN BDs.
 *
 * Drives the full host->device->firmware->device->host path.  Prints progress
 * on the FlexComm4 console and "USB ENUM OK" once SET_CONFIGURATION lands.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

/* ---- FlexComm4 / LPUART4 console ---------------------------------------- */
#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

static void putc_(char c)
{
    while (!(LP_STAT & STAT_TDRE)) {
    }
    LP_DATA = (uint8_t)c;
}
static void puts_(const char *s) { while (*s) { putc_(*s++); } }

/* ---- USBFS (KHCI) ------------------------------------------------------- */
#define USB_BASE   0x400DD000u
#define U8(o)      (*(volatile uint8_t *)(USB_BASE + (o)))
#define R_ISTAT    0x80
#define R_INTEN    0x84
#define R_STAT     0x90
#define R_CTL      0x94
#define R_ADDR     0x98
#define R_BDTPAGE1 0x9C
#define R_BDTPAGE2 0xB0
#define R_BDTPAGE3 0xB4
#define R_ENDPT0   0xC0
#define R_ENDPT(n) (0xC0 + (n) * 4)

#define ISTAT_USBRST  (1u << 0)
#define ISTAT_TOKDNE  (1u << 3)
#define CTL_USBENSOFEN (1u << 0)
#define ENDPT_EPHSHK  (1u << 0)
#define ENDPT_EPSTALL (1u << 1)
#define ENDPT_EPTXEN  (1u << 2)
#define ENDPT_EPRXEN  (1u << 3)

/* BD control-word bits. */
#define BD_STALL (1u << 2)
#define BD_DTS   (1u << 3)
#define BD_DATA1 (1u << 6)
#define BD_OWN   (1u << 7)
#define BD_BC(x) (((uint32_t)(x)) << 16)
#define BD_GET_BC(w)  (((w) >> 16) & 0x3FF)
#define BD_GET_PID(w) (((w) >> 2) & 0xF)
#define PID_SETUP 0xD

/* BDT in SRAM (512-byte aligned).  64 BDs (16 ep * 4), 8 bytes each. */
#define BDT_ADDR 0x20002000u
typedef struct { volatile uint32_t ctrl; volatile uint32_t addr; } bd_t;
#define BDT ((bd_t *)BDT_ADDR)
/* BD index = ep*4 + (tx?2:0) + odd. */
#define BD(ep, tx, odd) BDT[(ep) * 4 + ((tx) ? 2 : 0) + (odd)]

static uint8_t setup_buf[8];
static uint8_t ep0in_buf[64];
static uint8_t ep1out_buf[64];
static uint8_t ep1in_buf[64];

/* Controller ping-pong banks the engine will use next, mirrored here. */
static int rx_odd, tx_odd;          /* EP0 */
static int ep1_rx_odd, ep1_tx_odd;  /* EP1 bulk */

/* USB standard descriptors. */
static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0xC9, 0x1F,             /* idVendor  = 0x1FC9 (NXP)   */
    0x94, 0x00,             /* idProduct = 0x0094         */
    0x00, 0x01, 0, 0, 0, 1,
};
static const uint8_t cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,          /* configuration, wTotalLength=32 */
    9, 4, 0, 0, 2, 0xFF, 0, 0, 0,            /* interface (vendor), 2 endpoints */
    7, 5, 0x01, 2, 64, 0, 0,                 /* EP1 OUT, bulk, wMaxPacketSize=64 */
    7, 5, 0x81, 2, 64, 0, 0,                 /* EP1 IN,  bulk, wMaxPacketSize=64 */
};

static void arm_rx(int ep, int odd, void *buf, int len, int data1)
{
    BD(ep, 0, odd).addr = (uint32_t)(uintptr_t)buf;
    BD(ep, 0, odd).ctrl = BD_OWN | BD_DTS | (data1 ? BD_DATA1 : 0) | BD_BC(len);
}
static void arm_tx(int ep, int odd, const void *buf, int len, int data1)
{
    BD(ep, 1, odd).addr = (uint32_t)(uintptr_t)buf;
    BD(ep, 1, odd).ctrl = BD_OWN | BD_DTS | (data1 ? BD_DATA1 : 0) | BD_BC(len);
}

static void ep0_send(const uint8_t *data, int len, int wlen)
{
    if (len > wlen) {
        len = wlen;
    }
    if (len > 64) {
        len = 64;
    }
    for (int i = 0; i < len; i++) {
        ep0in_buf[i] = data[i];
    }
    arm_tx(0, tx_odd, ep0in_buf, len, 1);     /* IN data stage is DATA1 */
    tx_odd ^= 1;
}

static volatile int enum_done;

static void handle_setup(void)
{
    uint8_t bmreq = setup_buf[0];
    uint8_t breq  = setup_buf[1];
    uint16_t wval = setup_buf[2] | (setup_buf[3] << 8);
    uint16_t wlen = setup_buf[6] | (setup_buf[7] << 8);

    if (bmreq == 0x80 && breq == 6) {          /* GET_DESCRIPTOR (IN) */
        uint8_t type = wval >> 8;
        if (type == 1) {
            puts_("  GET_DESC device\r\n");
            ep0_send(dev_desc, sizeof(dev_desc), wlen);
        } else if (type == 2) {
            puts_("  GET_DESC config\r\n");
            ep0_send(cfg_desc, sizeof(cfg_desc), wlen);
        } else {
            U8(R_ENDPT0) |= ENDPT_EPSTALL;     /* unsupported descriptor */
        }
    } else if (bmreq == 0x00 && breq == 5) {   /* SET_ADDRESS */
        puts_("  SET_ADDRESS\r\n");
        U8(R_ADDR) = wval & 0x7F;
        ep0_send(0, 0, 0);                      /* zero-length status IN */
    } else if (bmreq == 0x00 && breq == 9) {   /* SET_CONFIGURATION */
        puts_("  SET_CONFIG\r\n");
        ep0_send(0, 0, 0);
        enum_done = 1;
    } else if ((bmreq & 0x80) && breq == 0) {  /* GET_STATUS -> 2 bytes */
        static const uint8_t st[2] = { 0, 0 };
        ep0_send(st, 2, wlen);
    } else {
        ep0_send(0, 0, 0);                      /* ack other no-data reqs */
    }
}

static void ep_config(void);            /* defined below; used on bus reset */

void usb_isr(void)
{
    uint8_t istat = U8(R_ISTAT);

    if (istat & ISTAT_USBRST) {          /* USB bus reset — re-init for a fresh
                                          * enumeration (lets a reused server
                                          * re-enumerate a new client). */
        U8(R_ISTAT) = ISTAT_USBRST;      /* W1C */
        U8(R_ADDR) = 0;                  /* revert to default address */
        ep_config();
    }
    if (istat & ISTAT_TOKDNE) {
        uint8_t stat = U8(R_STAT);
        int ep  = stat >> 4;
        int tx  = (stat >> 3) & 1;
        int odd = (stat >> 2) & 1;

        if (ep == 0 && !tx) {                   /* EP0 RX done (SETUP/OUT) */
            uint32_t w = BD(0, 0, odd).ctrl;
            if (BD_GET_PID(w) == PID_SETUP) {
                handle_setup();
            }
            rx_odd = odd ^ 1;
            arm_rx(0, rx_odd, setup_buf, 8, 0); /* re-arm for next SETUP */
        } else if (ep == 1 && !tx) {            /* EP1 bulk OUT -> echo on IN */
            uint32_t w = BD(1, 0, odd).ctrl;
            int bc = BD_GET_BC(w);
            for (int i = 0; i < bc && i < 64; i++) {
                ep1in_buf[i] = ep1out_buf[i];
            }
            arm_tx(1, ep1_tx_odd, ep1in_buf, bc, ep1_tx_odd);
            ep1_tx_odd ^= 1;
            ep1_rx_odd = odd ^ 1;
            arm_rx(1, ep1_rx_odd, ep1out_buf, 64, 0);  /* re-arm next OUT */
        }
        U8(R_ISTAT) = ISTAT_TOKDNE;             /* W1C; model services next */
    }
}

/* (Re)initialise the BDT pointer + EP0/EP1 buffer descriptors and endpoint
 * enables.  Run at boot and again on every USB bus reset so a reconnecting host
 * re-enumerates cleanly. */
static void ep_config(void)
{
    /* Point the controller at the BDT. */
    U8(R_BDTPAGE1) = (BDT_ADDR >> 8)  & 0xFF;
    U8(R_BDTPAGE2) = (BDT_ADDR >> 16) & 0xFF;
    U8(R_BDTPAGE3) = (BDT_ADDR >> 24) & 0xFF;

    /* Arm EP0 RX (even bank) and enable EP0 (handshake + TX + RX). */
    rx_odd = 0;
    tx_odd = 0;
    arm_rx(0, 0, setup_buf, 8, 0);
    U8(R_ENDPT0) = ENDPT_EPHSHK | ENDPT_EPTXEN | ENDPT_EPRXEN;

    /* Arm EP1 bulk OUT and enable EP1 (TX+RX) for the data-path echo. */
    ep1_rx_odd = 0;
    ep1_tx_odd = 0;
    arm_rx(1, 0, ep1out_buf, 64, 0);
    U8(R_ENDPT(1)) = ENDPT_EPHSHK | ENDPT_EPTXEN | ENDPT_EPRXEN;
}

void cpu0_main(void)
{
    LP_CTRL = CTRL_TE;
    puts_("USB enum test\r\n");

    ep_config();

    /* Enable TOKDNE + USBRST interrupts, NVIC IRQ 50, then enable the device. */
    U8(R_INTEN) = ISTAT_TOKDNE | ISTAT_USBRST;
    *(volatile uint32_t *)0xE000E104u = (1u << (50 - 32));  /* NVIC ISER1 */
    __asm__ volatile ("cpsie i");
    U8(R_CTL) = CTL_USBENSOFEN;                 /* device appears on the bus */

    while (!enum_done) {
    }
    puts_("USB ENUM OK\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[80] = {
    [0]  = (vec_t)0x20010000u,   /* initial MSP */
    [1]  = cpu0_main,            /* Reset_Handler */
    [16 + 50] = usb_isr,         /* exception 66 = IRQ 50 (USB0_FS) */
};
