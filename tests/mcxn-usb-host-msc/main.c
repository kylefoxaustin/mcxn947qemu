/*
 * MCXN947 USBFS/KHCI HOST-mode mass storage (MSC / Bulk-Only Transport).
 *
 * The MCX is the USB host: it enumerates an attached usb-storage device, SET_CONFIGURATION,
 * then reads block 0 with a real SCSI READ(10) over the BOT protocol -- CBW out on the bulk-OUT
 * endpoint, 512 bytes of data in on the bulk-IN endpoint (eight full-speed 64-byte packets),
 * then the CSW.  The 512 bytes are the ACTUAL contents of the backing disk image (a host file
 * the model cannot see or fabricate), so matching the block-0 signature proves the whole bulk
 * host data path end to end.  usb-storage block reads go through USB_RET_ASYNC, so this also
 * exercises the controller's port .complete async-completion path.
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
#define R_CTL      0x94
#define R_ADDR     0x98
#define R_BDTPAGE1 0x9C
#define R_TOKEN    0xA8
#define R_BDTPAGE2 0xB0
#define R_BDTPAGE3 0xB4
#define R_ENDPT(n) (0xC0 + (n) * 4)

#define ISTAT_TOKDNE  (1u << 3)
#define ISTAT_ATTACH  (1u << 6)
#define CTL_USBENSOFEN (1u << 0)
#define CTL_HOSTMODEEN (1u << 3)
#define CTL_RESET      (1u << 4)
#define EP_HSHK  (1u << 0)
#define EP_TXEN  (1u << 2)
#define EP_RXEN  (1u << 3)
#define EP_WOHUB (1u << 7)

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

#define EP_BULK_IN  1
#define EP_BULK_OUT 2

static uint8_t setup_buf[8]  __attribute__((aligned(4)));
static uint8_t xfer_buf[512] __attribute__((aligned(4)));
static uint8_t block0[512]   __attribute__((aligned(4)));

static int otx[4], orx[4];   /* per-endpoint ping-pong ODD */

static int khci_token(uint8_t token)
{
    int g = 40000000;
    U8(R_TOKEN) = token;
    while (!(U8(R_ISTAT) & ISTAT_TOKDNE) && g--) {
    }
    if (!(U8(R_ISTAT) & ISTAT_TOKDNE)) {
        return -1;
    }
    U8(R_ISTAT) = ISTAT_TOKDNE;
    return 0;
}

/* ---- EP0 control transfers (enumeration) ---- */
static int ctrl_setup(const uint8_t *s8)
{
    for (int i = 0; i < 8; i++) {
        setup_buf[i] = s8[i];
    }
    BD(0, 1, otx[0]).buf  = (uint32_t)(uintptr_t)setup_buf;
    BD(0, 1, otx[0]).ctrl = BD_OWN | BD_BC(8) | BD_DTS;
    if (khci_token(TOKEN(PID_SETUP, 0)) < 0) {
        return -1;
    }
    otx[0] ^= 1;
    return 0;
}

static int ctrl_in(uint8_t *out, int want)
{
    int acc = 0, data1 = 1;
    while (acc < want) {
        BD(0, 0, orx[0]).buf  = (uint32_t)(uintptr_t)xfer_buf;
        BD(0, 0, orx[0]).ctrl = BD_OWN | BD_BC(64) | BD_DTS | (data1 ? BD_DATA1 : 0);
        if (khci_token(TOKEN(PID_IN, 0)) < 0) {
            return -1;
        }
        int bc = BD_GET_BC(BD(0, 0, orx[0]).ctrl);
        orx[0] ^= 1; data1 ^= 1;
        for (int i = 0; i < bc && acc < want; i++) {
            out[acc++] = xfer_buf[i];
        }
        if (bc < 64) {
            break;
        }
    }
    return acc;
}

static int ctrl_status_out(void)
{
    BD(0, 1, otx[0]).buf  = (uint32_t)(uintptr_t)xfer_buf;
    BD(0, 1, otx[0]).ctrl = BD_OWN | BD_BC(0) | BD_DTS | BD_DATA1;
    int r = khci_token(TOKEN(PID_OUT, 0));
    otx[0] ^= 1;
    return r;
}

static int ctrl_status_in(void)
{
    BD(0, 0, orx[0]).buf  = (uint32_t)(uintptr_t)xfer_buf;
    BD(0, 0, orx[0]).ctrl = BD_OWN | BD_BC(64) | BD_DTS | BD_DATA1;
    int r = khci_token(TOKEN(PID_IN, 0));
    orx[0] ^= 1;
    return r;
}

static int get_dev_desc(uint8_t *out)
{
    static const uint8_t s8[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 18, 0x00 };
    if (ctrl_setup(s8) < 0 || ctrl_in(out, 18) < 0 || ctrl_status_out() < 0) {
        return -1;
    }
    return 0;
}

static int set_address(uint8_t a)
{
    uint8_t s8[8] = { 0x00, 0x05, a, 0, 0, 0, 0, 0 };
    return (ctrl_setup(s8) < 0) ? -1 : ctrl_status_in();
}

static int set_configuration(uint8_t c)
{
    uint8_t s8[8] = { 0x00, 0x09, c, 0, 0, 0, 0, 0 };
    return (ctrl_setup(s8) < 0) ? -1 : ctrl_status_in();
}

/* ---- bulk transfers ---- */
static int bulk_out(int ep, const uint8_t *buf, int len)
{
    int off = 0;
    do {
        int chunk = len - off; if (chunk > 64) chunk = 64;
        for (int i = 0; i < chunk; i++) {
            xfer_buf[i] = buf[off + i];
        }
        BD(ep, 1, otx[ep]).buf  = (uint32_t)(uintptr_t)xfer_buf;
        BD(ep, 1, otx[ep]).ctrl = BD_OWN | BD_BC(chunk) | BD_DTS | (otx[ep] ? BD_DATA1 : 0);
        if (khci_token(TOKEN(PID_OUT, ep)) < 0) {
            return -1;
        }
        otx[ep] ^= 1;
        off += chunk;
    } while (off < len);
    return off;
}

static int bulk_in(int ep, uint8_t *buf, int want)
{
    int acc = 0;
    while (acc < want) {
        BD(ep, 0, orx[ep]).buf  = (uint32_t)(uintptr_t)xfer_buf;
        BD(ep, 0, orx[ep]).ctrl = BD_OWN | BD_BC(64) | BD_DTS | (orx[ep] ? BD_DATA1 : 0);
        if (khci_token(TOKEN(PID_IN, ep)) < 0) {
            return -1;
        }
        int bc = BD_GET_BC(BD(ep, 0, orx[ep]).ctrl);
        orx[ep] ^= 1;
        for (int i = 0; i < bc && acc < want; i++) {
            buf[acc++] = xfer_buf[i];
        }
        if (bc < 64) {
            break;
        }
    }
    return acc;
}

/* BOT READ(10) of one 512-byte block at `lba` into out[512]. */
static int msc_read_block(uint32_t lba, uint8_t *out)
{
    uint8_t cbw[31] = { 0 };
    uint8_t csw[13];
    /* dCBWSignature "USBC" */
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
    /* dCBWTag */          cbw[4] = 0x78;
    /* dCBWDataTransferLength = 512 */ cbw[8] = 0x00; cbw[9] = 0x02;
    /* bmCBWFlags = 0x80 (data-IN) */  cbw[12] = 0x80;
    /* bCBWLUN 0, bCBWCBLength 10 */   cbw[14] = 10;
    /* CDB: READ(10) */
    cbw[15] = 0x28;
    cbw[17] = (lba >> 24) & 0xFF; cbw[18] = (lba >> 16) & 0xFF;
    cbw[19] = (lba >> 8) & 0xFF;  cbw[20] = lba & 0xFF;
    cbw[22] = 0x00; cbw[23] = 0x01;    /* transfer length = 1 block */

    if (bulk_out(EP_BULK_OUT, cbw, 31) < 0) {
        return -1;
    }
    if (bulk_in(EP_BULK_IN, out, 512) != 512) {
        return -2;
    }
    if (bulk_in(EP_BULK_IN, csw, 13) < 0) {
        return -3;
    }
    /* dCSWSignature "USBS" + bCSWStatus == 0 (passed) */
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53 || csw[12] != 0) {
        return -4;
    }
    return 0;
}

/* BOT WRITE(10) of one 512-byte block at `lba` from in[512].  The data phase is a bulk-OUT,
 * and usb-storage's block write is async -- so the CSW-IN completes through the controller's
 * port .complete path, exercising the async host completion for real. */
static int msc_write_block(uint32_t lba, const uint8_t *in)
{
    uint8_t cbw[31] = { 0 };
    uint8_t csw[13];
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;   /* "USBC" */
    cbw[4] = 0x79;                                                /* tag */
    cbw[8] = 0x00; cbw[9] = 0x02;                                 /* dCBWDataTransferLength = 512 */
    cbw[12] = 0x00;                                               /* bmCBWFlags = data-OUT */
    cbw[14] = 10;                                                 /* CBWCBLength */
    cbw[15] = 0x2A;                                               /* WRITE(10) */
    cbw[17] = (lba >> 24) & 0xFF; cbw[18] = (lba >> 16) & 0xFF;
    cbw[19] = (lba >> 8) & 0xFF;  cbw[20] = lba & 0xFF;
    cbw[22] = 0x00; cbw[23] = 0x01;                               /* 1 block */

    if (bulk_out(EP_BULK_OUT, cbw, 31) < 0) {
        return -1;
    }
    if (bulk_out(EP_BULK_OUT, in, 512) != 512) {
        return -2;
    }
    if (bulk_in(EP_BULK_IN, csw, 13) < 0) {
        return -3;
    }
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53 || csw[12] != 0) {
        return -4;
    }
    return 0;
}

void cpu0_main(void)
{
    uint8_t desc[18];
    static uint8_t wblk[512] __attribute__((aligned(4)));
    static uint8_t rblk[512] __attribute__((aligned(4)));
    int ok = 1, r, g;

    LP_CTRL = (1u << 19);
    puts_("USB-MSC test\r\n");

    U8(R_BDTPAGE1) = 0x20; U8(R_BDTPAGE2) = 0x00; U8(R_BDTPAGE3) = 0x20;
    U8(R_ADDR) = 0;
    U8(R_CTL) = CTL_HOSTMODEEN;
    U8(R_CTL) = CTL_HOSTMODEEN | CTL_USBENSOFEN;
    g = 20000000; while (!(U8(R_ISTAT) & ISTAT_ATTACH) && g--) {}
    ok &= !!(U8(R_ISTAT) & ISTAT_ATTACH);
    U8(R_ISTAT) = ISTAT_ATTACH;

    /* Bus reset -> DEFAULT state, then enable EP0 + the two bulk endpoints. */
    U8(R_CTL) = CTL_HOSTMODEEN | CTL_RESET;
    for (g = 0; g < 200000; g++) {}
    U8(R_CTL) = CTL_HOSTMODEEN | CTL_USBENSOFEN;
    U8(R_ENDPT(0)) = EP_HSHK | EP_TXEN | EP_RXEN | EP_WOHUB;
    U8(R_ENDPT(EP_BULK_IN))  = EP_HSHK | EP_TXEN | EP_RXEN | EP_WOHUB;
    U8(R_ENDPT(EP_BULK_OUT)) = EP_HSHK | EP_TXEN | EP_RXEN | EP_WOHUB;

    /* Enumerate: device descriptor, SET_ADDRESS, SET_CONFIGURATION. */
    ok &= (get_dev_desc(desc) == 0) && (desc[0] == 18) && (desc[1] == 1);
    puts_("  msc-dev vid="); puthex(desc[9], 2); puthex(desc[8], 2); puts_("\r\n");
    if (set_address(3) < 0) { ok = 0; }
    U8(R_ADDR) = 3;
    if (set_configuration(1) < 0) { ok = 0; }
    puts_("USB-MSC CONFIGURED\r\n");

    /* Read block 0.  A freshly-reset SCSI disk fails its FIRST command with a
     * UNIT ATTENTION (power-on reset), which the failed command itself clears --
     * so retry, exactly as a real MSC host does. */
    r = -1;
    for (int attempt = 0; attempt < 4 && r != 0; attempt++) {
        r = msc_read_block(0, block0);
    }
    ok &= (r == 0);
    puts_("  read rc="); puthex((uint32_t)(r & 0xff), 2);
    puts_(" sig="); for (int i = 0; i < 8; i++) putc_(block0[i] ? block0[i] : '.');
    puts_(" tail="); puthex(block0[511], 2); puthex(block0[510], 2);
    puthex(block0[509], 2); puthex(block0[508], 2); puts_("\r\n");

    /* Oracle: the backing image's block 0 begins "MCXN-USB" and ends DE AD BE EF. */
    static const char want[8] = "MCXN-USB";
    for (int i = 0; i < 8; i++) {
        if (block0[i] != (uint8_t)want[i]) { ok = 0; }
    }
    ok &= (block0[508] == 0xDE && block0[509] == 0xAD &&
           block0[510] == 0xBE && block0[511] == 0xEF);
    puts_(ok ? "USB-MSC READ OK\r\n" : "USB-MSC READ FAIL\r\n");

    /* WRITE block 1 with a signature, read it back, and verify.  The host harness
     * ALSO re-checks the backing file after exit -- an oracle the model cannot fake. */
    for (int i = 0; i < 512; i++) {
        wblk[i] = 0;
    }
    static const char wsig[16] = "MCX-WROTE-THIS!!";
    for (int i = 0; i < 16; i++) {
        wblk[i] = (uint8_t)wsig[i];
    }
    wblk[508] = 0xCA; wblk[509] = 0xFE; wblk[510] = 0xF0; wblk[511] = 0x0D;

    r = -1;
    for (int attempt = 0; attempt < 4 && r != 0; attempt++) {
        r = msc_write_block(1, wblk);
    }
    ok &= (r == 0);
    puts_("  write rc="); puthex((uint32_t)(r & 0xff), 2); puts_("\r\n");

    r = -1;
    for (int attempt = 0; attempt < 4 && r != 0; attempt++) {
        r = msc_read_block(1, rblk);
    }
    ok &= (r == 0);
    for (int i = 0; i < 16; i++) {
        if (rblk[i] != (uint8_t)wsig[i]) { ok = 0; }
    }
    ok &= (rblk[508] == 0xCA && rblk[509] == 0xFE && rblk[510] == 0xF0 && rblk[511] == 0x0D);
    puts_("  readback sig="); for (int i = 0; i < 12; i++) putc_(rblk[i] ? rblk[i] : '.');
    puts_("\r\n");

    puts_(ok ? "USB-MSC RW OK\r\n" : "USB-MSC RW FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
