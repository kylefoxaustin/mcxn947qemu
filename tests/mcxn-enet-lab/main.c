/*
 * MCXN947 cross-board ENET L2 lab node — lab3 v2 nonce-beacon (fleet-agreed body).
 *
 * Brings up the ENET-QoS MAC + descriptor rings (no MAC loopback), then on a shared
 * L2 segment (or through the RT1180 switched fabric) it:
 *   (a) periodically BROADCASTS a 64-byte v2 beacon from the distinct MCX source MAC, and
 *   (b) RECEIVES peer beacons and BODY-VERIFIES them per the fleet freshness contract.
 *
 * v2 beacon body (byte-exact, fleet-agreed — see rt1180 tools/netc-eth-lab3.sh):
 *   [0..5]   dst MAC        = broadcast
 *   [6..11]  src MAC        = this node
 *   [12..13] ethertype      = 0x88B5 (big-endian)
 *   [14..17] MAGIC          = B5 B6 B7 C0
 *   [18..19] self-ethertype = 0x88B5 (== [12..13])
 *   [20..23] SEQUENCE       = uint32 BE, ++ before EVERY frame (per-frame monotonic)
 *   [24..27] INCARNATION    = uint32 BE, per-BOOT nonce (REAL entropy, see below)
 *   [28..63] FILL           = 0x5A in every byte
 *
 * THE INCARNATION MUST BE A REAL PER-BOOT NONCE — different every boot, never a constant
 * and never derived from the (deterministic under -icount) instruction stream, or it looks
 * fresh and never is and poisons every peer's freshness logic.  We source it from ELS
 * PRNG_DATOUT, which is seeded at reset from qemu_guest_getrandom (real host entropy per
 * boot; reproducible only under -seed) — the honest DTRNG path, not a fabricated stream.
 *
 * RX freshness contract (VERIFIED vs replay vs reboot), per (peer, incarnation):
 *   magic + self-ET + fill must match; then seq must advance.  seq backwards, SAME
 *   incarnation -> REPLAY (stale buffer) -> CONDEMN.  seq backwards, NEW incarnation ->
 *   a real REBOOT -> reset the counter and COUNT it.  Peer identity is the SRC MAC (not
 *   the ethertype: the two-MCX self-test runs two nodes that share ET 0x88B5).
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

/* ELS EdgeLock — real per-boot entropy (seeded from qemu_guest_getrandom at reset). */
#define ELS_BASE          0x40054000u
#define ELS_STATUS        (*(volatile uint32_t *)(ELS_BASE + 0x00))
#define ELS_PRNG_DATOUT   (*(volatile uint32_t *)(ELS_BASE + 0x5C))
#define ELS_STATUS_PRNG_RDY (1u << 3)

#define ENET 0x40100000u
#define MAC_CONFIG  (*(volatile uint32_t *)(ENET + 0x000))
#define DMA_MODE    (*(volatile uint32_t *)(ENET + 0x1000))
#define TX_CTRL     (*(volatile uint32_t *)(ENET + 0x1104))
#define RX_CTRL     (*(volatile uint32_t *)(ENET + 0x1108))
#define TXDESC_LIST (*(volatile uint32_t *)(ENET + 0x1114))
#define RXDESC_LIST (*(volatile uint32_t *)(ENET + 0x111C))
#define TXDESC_TAIL (*(volatile uint32_t *)(ENET + 0x1120))
#define RXDESC_TAIL (*(volatile uint32_t *)(ENET + 0x1128))
#define TXRING_LEN  (*(volatile uint32_t *)(ENET + 0x112C))
#define RXRING_LEN  (*(volatile uint32_t *)(ENET + 0x1130))
#define DMA_INT_EN  (*(volatile uint32_t *)(ENET + 0x1134))
#define DMA_STATUS  (*(volatile uint32_t *)(ENET + 0x1160))

#define MAC_RE 0x1u
#define MAC_TE 0x2u
#define DMA_SWR 0x1u
#define TX_ST 0x1u
#define RX_SR 0x1u
#define STAT_RI 0x40u
#define INT_RIE 0x40u
#define INT_NIE 0x8000u

#define TDES2_IOC 0x80000000u
#define TDES3_OWN 0x80000000u
#define TDES3_FD  0x20000000u
#define TDES3_LD  0x10000000u
#define RDES3_OWN 0x80000000u
#define RDES3_IOC 0x40000000u
#define RDES3_BUF1V 0x01000000u

#define TXDESC 0x20008000u
#define TXBUF  0x20008100u
#define RXDESC 0x20008200u
#define RXBUF  0x20008300u
#define FRAME_LEN 64
#define ETHERTYPE 0x88B5u   /* IEEE local-experimental EtherType 1 (this node) */

#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MEM8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

#define NVIC_ISER4 (*(volatile uint32_t *)0xE000E110u)
#define ENET_IRQ 139

/* Distinct MCX source MAC (locally administered). */
static const uint8_t MCX_MAC[6] = { 0x02, 0x4D, 0x43, 0x58, 0x00, 0x01 };

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

static void puthex2(uint8_t v)
{
    const char *h = "0123456789abcdef";
    putc_(h[v >> 4]);
    putc_(h[v & 0xF]);
}

static void puthex8(uint32_t v)
{
    puthex2((v >> 24) & 0xFF);
    puthex2((v >> 16) & 0xFF);
    puthex2((v >> 8) & 0xFF);
    puthex2(v & 0xFF);
}

static void put_mac(uintptr_t p)
{
    int i;
    for (i = 0; i < 6; i++) {
        puthex2(MEM8(p + i));
        if (i < 5) {
            putc_(':');
        }
    }
}

static volatile uint32_t got;

void enet_handler(void)
{
    uint32_t st = DMA_STATUS;
    if (st & STAT_RI) {
        got = 1;
    }
    DMA_STATUS = st & STAT_RI;
}

/* ---- per-peer freshness state (identity = src MAC) ------------------------ */
struct peer {
    uint8_t  mac[6];
    uint32_t inc;
    uint32_t seq;
    int      used;
    int      verified;   /* has produced >=1 body-verified frame */
};
static struct peer peers[4];
static int distinct_verified;
static int announced_pass;

static int mac_eq(uintptr_t p, const uint8_t *m)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (MEM8(p + i) != m[i]) {
            return 0;
        }
    }
    return 1;
}

static struct peer *find_peer(uintptr_t macp)
{
    int i, j;
    for (i = 0; i < 4; i++) {
        if (peers[i].used) {
            int same = 1;
            for (j = 0; j < 6; j++) {
                if (peers[i].mac[j] != MEM8(macp + j)) {
                    same = 0;
                    break;
                }
            }
            if (same) {
                return &peers[i];
            }
        }
    }
    for (i = 0; i < 4; i++) {
        if (!peers[i].used) {
            for (j = 0; j < 6; j++) {
                peers[i].mac[j] = MEM8(macp + j);
            }
            peers[i].used = 1;
            peers[i].verified = 0;
            return &peers[i];
        }
    }
    return &peers[3];
}

static uint32_t tx_seq;
static uint32_t incarnation;

static void build_beacon(void)
{
    int i;
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + i) = 0xFF;               /* dst = broadcast */
    }
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + 6 + i) = MCX_MAC[i];     /* src = MCX */
    }
    MEM8(TXBUF + 12) = (ETHERTYPE >> 8) & 0xFF;
    MEM8(TXBUF + 13) = ETHERTYPE & 0xFF;
    MEM8(TXBUF + 14) = 0xB5; MEM8(TXBUF + 15) = 0xB6;   /* MAGIC B5B6B7C0 */
    MEM8(TXBUF + 16) = 0xB7; MEM8(TXBUF + 17) = 0xC0;
    MEM8(TXBUF + 18) = (ETHERTYPE >> 8) & 0xFF;         /* self-ethertype */
    MEM8(TXBUF + 19) = ETHERTYPE & 0xFF;
    /* [20..23] sequence written per-frame in arm_tx() */
    MEM8(TXBUF + 24) = (incarnation >> 24) & 0xFF;      /* incarnation (per-boot) */
    MEM8(TXBUF + 25) = (incarnation >> 16) & 0xFF;
    MEM8(TXBUF + 26) = (incarnation >> 8) & 0xFF;
    MEM8(TXBUF + 27) = incarnation & 0xFF;
    for (i = 28; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = 0x5A;               /* fill */
    }
    MEM32(TXDESC + 0) = TXBUF;
    MEM32(TXDESC + 4) = 0;
}

static void arm_tx(void)
{
    ++tx_seq;                                 /* ++ before EVERY frame */
    MEM8(TXBUF + 20) = (tx_seq >> 24) & 0xFF;
    MEM8(TXBUF + 21) = (tx_seq >> 16) & 0xFF;
    MEM8(TXBUF + 22) = (tx_seq >> 8) & 0xFF;
    MEM8(TXBUF + 23) = tx_seq & 0xFF;
    MEM32(TXDESC + 8) = TDES2_IOC | FRAME_LEN;
    MEM32(TXDESC + 12) = TDES3_OWN | TDES3_FD | TDES3_LD | FRAME_LEN;
    TXDESC_TAIL = TXDESC + 16;
}

static void rearm_rx(void)
{
    MEM32(RXDESC + 12) = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;
    RXDESC_TAIL = RXDESC + 16;
}

/* Validate one received frame's v2 body; returns 1 if it is a well-formed peer beacon. */
static int body_ok(void)
{
    int i;
    if (MEM8(RXBUF + 14) != 0xB5 || MEM8(RXBUF + 15) != 0xB6 ||
        MEM8(RXBUF + 16) != 0xB7 || MEM8(RXBUF + 17) != 0xC0) {
        return 0;                             /* magic mismatch */
    }
    if (MEM8(RXBUF + 18) != MEM8(RXBUF + 12) ||
        MEM8(RXBUF + 19) != MEM8(RXBUF + 13)) {
        return 0;                             /* self-ethertype != ethertype */
    }
    for (i = 28; i < FRAME_LEN; i++) {
        if (MEM8(RXBUF + i) != 0x5A) {
            return 0;                         /* fill corrupt */
        }
    }
    return 1;
}

static void handle_rx(void)
{
    uint32_t et = ((uint32_t)MEM8(RXBUF + 12) << 8) | MEM8(RXBUF + 13);
    uint32_t seq = 0, inc = 0;
    int v2 = body_ok();
    struct peer *p;

    if (v2) {
        seq = ((uint32_t)MEM8(RXBUF + 20) << 24) | ((uint32_t)MEM8(RXBUF + 21) << 16) |
              ((uint32_t)MEM8(RXBUF + 22) << 8)  | MEM8(RXBUF + 23);
        inc = ((uint32_t)MEM8(RXBUF + 24) << 24) | ((uint32_t)MEM8(RXBUF + 25) << 16) |
              ((uint32_t)MEM8(RXBUF + 26) << 8)  | MEM8(RXBUF + 27);
    }

    /* Drop our OWN looped-back beacon silently (mcast IP_MULTICAST_LOOP): self ==
     * our src MAC AND our per-boot incarnation.  A second node sharing our MAC in the
     * two-MCX self-test has a DIFFERENT incarnation, so it is (correctly) a peer. */
    if (v2 && mac_eq(RXBUF + 6, MCX_MAC) && inc == incarnation) {
        return;
    }

    puts_("ENET-LAB rx: ethertype 0x");
    puthex2((et >> 8) & 0xFF); puthex2(et & 0xFF);
    puts_(" src "); put_mac(RXBUF + 6);

    if (!v2) {
        puts_(" FOREIGN (ignored)\r\n");      /* non-v2 / not a lab beacon */
        return;
    }

    p = find_peer(RXBUF + 6);

    if (!p->verified) {
        p->inc = inc; p->seq = seq; p->verified = 1;
        distinct_verified++;
        puts_(" VERIFIED seq="); puthex8(seq);
        puts_(" inc=0x"); puthex8(inc); puts_("\r\n");
    } else if (inc == p->inc) {
        if (seq > p->seq) {
            p->seq = seq;                     /* fresh advance — stay verified, terse */
            puts_(" ok\r\n");
        } else {
            puts_(" REPLAY (seq<=last, same inc) CONDEMNED\r\n");
        }
    } else {
        p->inc = inc; p->seq = seq;           /* new incarnation = real reboot */
        puts_(" REBOOT (new inc=0x"); puthex8(inc); puts_(") counted\r\n");
    }

    if (!announced_pass && distinct_verified >= 2) {
        announced_pass = 1;
        puts_("ENET-LAB3 PASS: 2 peers body-VERIFIED\r\n");
    }
}

void cpu0_main(void)
{
    int i;
    volatile int d;

    LP_CTRL = CTRL_TE;

    /* Real per-boot incarnation nonce from ELS PRNG (host entropy at reset). */
    while (!(ELS_STATUS & ELS_STATUS_PRNG_RDY)) {
    }
    incarnation = ELS_PRNG_DATOUT;

    puts_("ENET-LAB up: MCX v2 beacon 0x88B5, incarnation=0x");
    puthex8(incarnation); puts_("\r\n");

    DMA_MODE = DMA_SWR;
    while (DMA_MODE & DMA_SWR) {
    }

    build_beacon();

    MEM32(RXDESC + 0) = RXBUF;
    MEM32(RXDESC + 4) = 0;
    MEM32(RXDESC + 8) = 0;
    MEM32(RXDESC + 12) = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;

    TXDESC_LIST = TXDESC; TXRING_LEN = 0;
    RXDESC_LIST = RXDESC; RXRING_LEN = 0;
    RX_CTRL = RX_SR;
    TX_CTRL = TX_ST;
    MAC_CONFIG = MAC_TE | MAC_RE;   /* no loopback: frames go out the wire */
    DMA_INT_EN = INT_RIE | INT_NIE;

    NVIC_ISER4 = (1u << (ENET_IRQ - 128));
    __asm__ volatile ("cpsie i");

    rearm_rx();

    for (i = 0; ; i++) {
        arm_tx();
        if (got) {
            got = 0;
            handle_rx();
            rearm_rx();
        }
        for (d = 0; d < 60000; d++) {
        }
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[160] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + ENET_IRQ] = enet_handler,  /* exception 155 = IRQ 139 */
};
