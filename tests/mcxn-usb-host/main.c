/*
 * MCXN947 USBFS/KHCI HOST-mode enumeration.
 *
 * The MCX is the USB HOST here: it enumerates a device (-device usb-kbd) attached to the
 * controller's own usb-bus.  A bare-metal KHCI host driver enables host mode, waits for the
 * ATTACH, and drives real control transfers to the attached device — SETUP out, IN data,
 * OUT status — reading its 18-byte DEVICE descriptor, then SET_ADDRESS and re-reading the
 * descriptor at the new address.  Every token the guest writes launches one transaction that
 * the model executes against the real QEMU usb-kbd via usb_handle_packet, so the bytes read
 * back are the ACTUAL device's descriptor, not a fabricated reply.
 *
 * The KHCI ping-pong (ODD) bank alternates per direction per transaction; the driver tracks
 * its own ODD counters in lockstep with the controller (both reset to 0, both toggle on each
 * transaction) so it arms the bank the controller will read.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#define LP 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LP + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LP + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LP + 0x1C))
static void putc_(char c) { while (!(LP_STAT & (1u << 23))) {} LP_DATA = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) {
    int d = (v >> (i * 4)) & 0xF; putc_(d < 10 ? '0' + d : 'a' + d - 10); } }

#define USB 0x400DD000u
#define U8(o) (*(volatile uint8_t *)(USB + (o)))
#define R_ISTAT    0x80
#define R_STAT     0x90
#define R_CTL      0x94
#define R_ADDR     0x98
#define R_BDTPAGE1 0x9C
#define R_TOKEN    0xA8
#define R_BDTPAGE2 0xB0
#define R_BDTPAGE3 0xB4
#define R_ENDPT0   0xC0

#define ISTAT_TOKDNE  (1u << 3)
#define ISTAT_ATTACH  (1u << 6)
#define CTL_USBENSOFEN (1u << 0)
#define CTL_HOSTMODEEN (1u << 3)
#define CTL_RESET      (1u << 4)
#define ENDPT_EPHSHK  (1u << 0)
#define ENDPT_EPTXEN  (1u << 2)
#define ENDPT_EPRXEN  (1u << 3)
#define ENDPT_HOSTWOHUB (1u << 7)

#define BD_DTS   (1u << 3)
#define BD_DATA1 (1u << 6)
#define BD_OWN   (1u << 7)
#define BD_BC(x) (((uint32_t)(x)) << 16)
#define BD_GET_BC(w) (((w) >> 16) & 0x3FF)

#define PID_OUT   0x1
#define PID_IN    0x9
#define PID_SETUP 0xD
#define TOKEN(pid, ep) (uint8_t)(((pid) << 4) | (ep))

typedef struct { uint32_t ctrl; uint32_t buf; } bd_t;
#define BDT ((volatile bd_t *)0x20002000u)
#define BD(ep, tx, odd) BDT[(ep) * 4 + ((tx) ? 2 : 0) + (odd)]

static uint8_t setup_buf[8] __attribute__((aligned(4)));
static uint8_t data_buf[64] __attribute__((aligned(4)));

/* ping-pong ODD, tracked in lockstep with the controller (EP0 only). */
static int otx, orx;

/* Launch one token and wait for TOKDNE; returns the retired BD's byte count (-1 on timeout). */
static int khci_token(uint8_t token)
{
    int g = 20000000;
    U8(R_TOKEN) = token;
    while (!(U8(R_ISTAT) & ISTAT_TOKDNE) && g--) {
    }
    if (!(U8(R_ISTAT) & ISTAT_TOKDNE)) {
        return -1;                              /* controller never completed the token */
    }
    U8(R_ISTAT) = ISTAT_TOKDNE;                 /* W1C ack -> STAT advances */
    return 0;
}

/* SETUP stage: arm EP0 TX (DATA0) with the 8-byte request, send a SETUP token. */
static int ctrl_setup(const uint8_t *s8)
{
    for (int i = 0; i < 8; i++) {
        setup_buf[i] = s8[i];
    }
    BD(0, 1, otx).buf  = (uint32_t)(uintptr_t)setup_buf;
    BD(0, 1, otx).ctrl = BD_OWN | BD_BC(8) | BD_DTS;    /* DATA0 */
    if (khci_token(TOKEN(PID_SETUP, 0)) < 0) {
        return -1;
    }
    otx ^= 1;
    return 0;
}

/* IN data stage: accumulate into out[] until a short packet or `want` bytes. DATA1 first. */
static int ctrl_in(uint8_t *out, int want)
{
    int acc = 0, data1 = 1;
    while (acc < want) {
        BD(0, 0, orx).buf  = (uint32_t)(uintptr_t)data_buf;
        BD(0, 0, orx).ctrl = BD_OWN | BD_BC(64) | BD_DTS | (data1 ? BD_DATA1 : 0);
        if (khci_token(TOKEN(PID_IN, 0)) < 0) {
            return -1;
        }
        int bc = BD_GET_BC(BD(0, 0, orx).ctrl);
        orx ^= 1;
        data1 ^= 1;
        for (int i = 0; i < bc && acc < want; i++) {
            out[acc++] = data_buf[i];
        }
        if (bc < 64) {
            break;                              /* short packet ends the data stage */
        }
    }
    return acc;
}

/* OUT status stage: zero-length DATA1 OUT. */
static int ctrl_status_out(void)
{
    BD(0, 1, otx).buf  = (uint32_t)(uintptr_t)data_buf;
    BD(0, 1, otx).ctrl = BD_OWN | BD_BC(0) | BD_DTS | BD_DATA1;
    if (khci_token(TOKEN(PID_OUT, 0)) < 0) {
        return -1;
    }
    otx ^= 1;
    return 0;
}

/* IN status stage (for a no-data control transfer like SET_ADDRESS): zero-length DATA1 IN. */
static int ctrl_status_in(void)
{
    BD(0, 0, orx).buf  = (uint32_t)(uintptr_t)data_buf;
    BD(0, 0, orx).ctrl = BD_OWN | BD_BC(64) | BD_DTS | BD_DATA1;
    if (khci_token(TOKEN(PID_IN, 0)) < 0) {
        return -1;
    }
    orx ^= 1;
    return 0;
}

int get_device_descriptor(uint8_t *out)
{
    static const uint8_t s8[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00 };
    if (ctrl_setup(s8) < 0) {
        return -1;
    }
    int n = ctrl_in(out, 18);
    if (n < 0) {
        return -1;
    }
    if (ctrl_status_out() < 0) {
        return -1;
    }
    return n;
}

int set_address(uint8_t addr)
{
    uint8_t s8[8] = { 0x00, 0x05, addr, 0x00, 0x00, 0x00, 0x00, 0x00 };
    if (ctrl_setup(s8) < 0) {
        return -1;
    }
    /* no data stage; the status stage is a zero-length IN */
    return ctrl_status_in();
}

void cpu0_main(void)
{
    uint8_t desc[18];
    int ok = 1, n, g;

    LP_CTRL = (1u << 19);
    puts_("USB-HOST test\r\n");

    /* BDT base 0x2000_2000 -> BDTPAGE1=0x20, PAGE2=0x00, PAGE3=0x20. */
    U8(R_BDTPAGE1) = 0x20;
    U8(R_BDTPAGE2) = 0x00;
    U8(R_BDTPAGE3) = 0x20;
    U8(R_ADDR) = 0;
    U8(R_ENDPT0) = ENDPT_EPHSHK | ENDPT_EPTXEN | ENDPT_EPRXEN | ENDPT_HOSTWOHUB;

    /* Enable host mode + SOF; a device is already plugged into our port. */
    U8(R_CTL) = CTL_HOSTMODEEN;
    U8(R_CTL) = CTL_HOSTMODEEN | CTL_USBENSOFEN;

    g = 20000000;
    while (!(U8(R_ISTAT) & ISTAT_ATTACH) && g--) {
    }
    ok &= !!(U8(R_ISTAT) & ISTAT_ATTACH);
    U8(R_ISTAT) = ISTAT_ATTACH;
    puts_(ok ? "USB-HOST ATTACH\r\n" : "USB-HOST NO-ATTACH\r\n");

    /* Drive a USB bus reset so the device leaves ATTACHED for the addressable
     * DEFAULT state (address 0), then re-enable the port. */
    U8(R_CTL) = CTL_HOSTMODEEN | CTL_RESET;
    for (g = 0; g < 200000; g++) {
    }
    U8(R_CTL) = CTL_HOSTMODEEN | CTL_USBENSOFEN;
    U8(R_ENDPT0) = ENDPT_EPHSHK | ENDPT_EPTXEN | ENDPT_EPRXEN | ENDPT_HOSTWOHUB;

    /* GET_DESCRIPTOR(device) at address 0. */
    n = get_device_descriptor(desc);
    ok &= (n == 18) && (desc[0] == 18) && (desc[1] == 0x01);   /* bLength / DEVICE */
    /* The attached device is QEMU's usb-kbd (idVendor 0x0627, idProduct 0x0001).
     * Asserting it proves the bytes are the REAL device's descriptor, not a
     * fabricated reply -- the whole point of driving usb_handle_packet. */
    ok &= (desc[8] == 0x27) && (desc[9] == 0x06) &&
          (desc[10] == 0x01) && (desc[11] == 0x00);
    puts_("  devdesc len="); puthex(n, 2);
    puts_(" vid="); puthex(desc[9], 2); puthex(desc[8], 2);
    puts_(" pid="); puthex(desc[11], 2); puthex(desc[10], 2); puts_("\r\n");

    /* SET_ADDRESS(5), then re-read the descriptor at the new address. */
    if (set_address(5) < 0) {
        ok = 0;
    }
    U8(R_ADDR) = 5;
    n = get_device_descriptor(desc);
    ok &= (n == 18) && (desc[0] == 18) && (desc[1] == 0x01);
    puts_("  re-read@addr5 len="); puthex(n, 2); puts_("\r\n");

    puts_(ok ? "USB-HOST ENUM OK\r\n" : "USB-HOST ENUM FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
