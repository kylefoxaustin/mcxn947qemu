/*
 * MCXN947 three-node raw-L2 lab node (the fleet's Cortex-M + A55 cross-check).
 *
 * All three nodes join ONE L2 segment (a QEMU `-nic socket,mcast=` group), each
 * broadcasts its own distinct EtherType, and each must observe BOTH of the other
 * two before it declares PASS:
 *
 *      mcxn947   0x88B5      (this node, by default)
 *      rt1180    0x88B6
 *      imx95     0x88B7
 *
 * Distinct EtherTypes are the point: "I saw the other TWO" is then asserted from
 * the frames themselves, not inferred from a source MAC — and a node cannot
 * satisfy its own check with its own echoed broadcast (a real risk on a mcast
 * segment, where the socket backend can hand you back your own traffic).  We
 * additionally ignore any frame carrying our own EtherType or source MAC.
 *
 * It broadcasts FOREVER and never times out (the fleet's retry-forever rule):
 * holobench co-launches the nodes, and a Linux peer can take minutes to bring its
 * interface up while a bare-metal M33 is on the wire in milliseconds.  Only a
 * malformed frame is a hard fail; silence just means "peer not up yet".
 *
 * Build-time overrides let one firmware stand in for any node, so the segment can
 * be self-tested with three MCX instances before the real siblings join:
 *   -DMY_ETHERTYPE=0x88B6 -DPEER_A=0x88B5 -DPEER_B=0x88B7 -DMY_MAC_LSB=0x02
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>

#ifndef MY_ETHERTYPE
#define MY_ETHERTYPE 0x88B5u    /* this node: MCXN947 */
#endif
#ifndef PEER_A
#define PEER_A 0x88B6u          /* rt1180 */
#endif
#ifndef PEER_B
#define PEER_B 0x88B7u          /* imx95  */
#endif
/*
 * ⭐ A FOURTH PEER, BECAUSE AN ABSENCE OF REJECTION IS NOT AN ACCEPTANCE.
 *
 * This firmware knew about exactly THREE ethertypes.  When 91emulator's imx91 node
 * (0x88B8) joined holobench's lab, `is_beacon_et()` returned false on every one of its
 * frames, so `frame_ok()` bailed at the first line and NEVER READ THEIR BODY.
 *
 * holobench's interop matrix then showed "mcx never rejected imx91" -- and that was read
 * as mcx ACCEPTING them.  It was not.
 *
 *   ⭐ "mcx NEVER REJECTED imx91" AND "mcx NEVER LOOKED AT imx91" ARE THE SAME CELL.
 *     (91emulator, catching their own rule -- "we never fired on IPv6 and there was no
 *      IPv6 are the same log" -- being missed in someone else's table.)
 *
 * Their beacon had never been read by an implementation they did not author, which is the
 * only kind of oracle that can see a body you got wrong.  Now it has one.
 *
 * ⚠ AND THE HOLE WAS OPENED BY MY OWN IPv6 FIX.  Before is_beacon_et() I body-checked
 *   EVERY frame, so I would at least have EVALUATED 0x88B8.  I traded "shouts at IPv6"
 *   for "blind to a fourth peer" -- the same root cause both times: a hardcoded world.
 *   PEER_C is the seam; set it and the node sees one more.
 */
#ifndef PEER_C
#define PEER_C 0                /* 0 = absent.  imx91 is 0x88B8. */
#endif
#ifndef MY_MAC_LSB
#define MY_MAC_LSB 0x01
#endif

#define LPUART4_BASE 0x400B4000u
#define LP_STAT (*(volatile uint32_t *)(LPUART4_BASE + 0x14))
#define LP_CTRL (*(volatile uint32_t *)(LPUART4_BASE + 0x18))
#define LP_DATA (*(volatile uint32_t *)(LPUART4_BASE + 0x1C))
#define CTRL_TE   (1u << 19)
#define STAT_TDRE (1u << 23)

#define ENET 0x40100000u
#define MAC_CONFIG  (*(volatile uint32_t *)(ENET + 0x0000))
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

/* holobench's checkable-body spec -- the same on every node of the lab. */
#define BEACON_MAGIC 0xB5B6B7C0u
#define BEACON_FILL  0x5Au

/*
 * ⭐ THE INFORMATION TO TELL A REBOOT FROM A REPLAY WAS NEVER ON THE WIRE.  YOU CANNOT
 *   RECOVER IT WITH A SMARTER RULE; YOU HAVE TO ADD IT.   (rt1180emulator / 95emulator)
 *
 * A replayed frame and a genuine reboot are IDENTICAL on the sequence number alone: a
 * peer that restarts resets seq to 1, which reads as seq<=last -- exactly a replay.  A
 * bigger-backwards-jump rule cannot save it either: a stalled RX ring can hand back seq 1
 * too.  The only fix is a value the peer CHANGES ACROSS ITS OWN BOOTS and nothing else --
 * a per-boot incarnation.  Ratified fleet body (identical on rt1180 / 95 / imx91 / here):
 *
 *   [14..17] magic 0xB5B6B7C0     [20..23] seq (BE)         [28..63] 0x5A fill
 *   [18..19] self-ethertype       [24..27] INCARNATION (BE, per-boot)
 *
 * The incarnation comes from the ELS DTRNG (ELS_PRNG_DATOUT), which reseeds from physical
 * entropy every power-on -- so it is genuinely per-boot AND per-instance.  0x5A5A5A5A is
 * the SENTINEL a legacy/v1 node produces by leaving [24..27] as fill: it means "no
 * incarnation", and TX guarantees a real nonce never collides with it.
 */
#define ELS_PRNG_DATOUT  (*(volatile uint32_t *)0x4005405Cu)  /* fresh per-boot entropy word */
#define BEACON_SENTINEL  0x5A5A5A5Au    /* [24..27]==fill => legacy peer, no incarnation */

#define MEM32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define MEM8(a)  (*(volatile uint8_t  *)(uintptr_t)(a))

/*
 * ⭐ IF A PEER SET IS A CONSTANT, EVERY FUTURE NODE IS A FIRMWARE RELEASE. (95emulator)
 *
 * PEER_A/B/C are compile-time, so when imx91 (0x88B8) joined the lab this firmware went
 * BLIND to it -- and the fix was a rebuild, a restage, a new hash and a new announcement.
 * 95's node takes its peer list from argv; a bare-metal M33 has no argv, but it has a
 * memory map and QEMU has `-device loader`.
 *
 * So: a PEER TABLE in SRAM, seeded from the launch line, read once at boot.
 *
 *     -device loader,addr=0x20007f00,data=0x52454550,data-len=4   <- 'PEER' (LE)
 *     -device loader,addr=0x20007f04,data=3,data-len=4            <- count
 *     -device loader,addr=0x20007f08,data=0x88b6,data-len=4       <- peers...
 *     -device loader,addr=0x20007f0c,data=0x88b7,data-len=4
 *     -device loader,addr=0x20007f10,data=0x88b8,data-len=4
 *
 * ⭐ ONE PINNED IMAGE, ANY SEGMENT.  The topology lives in the launch line where the lab
 *   can SEE it; the binary stays a constant the lab can PIN.  (91emulator's rule, which I
 *   agreed with and then did not implement -- and it cost the fleet a stale artifact.)
 *
 * With no table present the compile-time defaults stand, so every existing suite is
 * unchanged and a node with no launch-line config still works.
 */
#define PEERTAB      0x20007F00u
#define PEERTAB_MAGIC 0x52454550u      /* 'PEER', little-endian */
#define PEERTAB_MAX  8u

static uint32_t peer_et[PEERTAB_MAX];
static uint32_t peer_n;

static void peers_init(void)
{
    uint32_t i;

    if (MEM32(PEERTAB) == PEERTAB_MAGIC) {
        uint32_t n = MEM32(PEERTAB + 4);

        if (n > PEERTAB_MAX) {
            n = PEERTAB_MAX;           /* a table that overruns is a table we truncate */
        }
        for (i = 0; i < n; i++) {
            peer_et[i] = MEM32(PEERTAB + 8 + 4 * i);
        }
        peer_n = n;
        return;
    }

    /* No table: fall back to the compile-time set. */
    peer_et[0] = PEER_A;
    peer_et[1] = PEER_B;
    peer_n = 2;
    if (PEER_C) {
        peer_et[2] = PEER_C;
        peer_n = 3;
    }
}

/* Which slot is this peer, or -1?  Everything downstream is indexed by SLOT, so a peer
 * set of any size costs no code -- which is the entire point of a runtime table. */
static int peer_idx(uint32_t et)
{
    uint32_t i;

    for (i = 0; i < peer_n; i++) {
        if (et == peer_et[i]) {
            return (int)i;
        }
    }
    return -1;
}

static int is_peer(uint32_t et)
{
    return peer_idx(et) >= 0;
}


#define NVIC_ISER4 (*(volatile uint32_t *)0xE000E110u)
#define ENET_IRQ 139

/* Locally-administered source MAC, unique per node via MY_MAC_LSB. */
static const uint8_t MY_MAC[6] = { 0x02, 0x4D, 0x43, 0x58, 0x00, MY_MAC_LSB };

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
    static const char h[] = "0123456789abcdef";

    putc_(h[(v >> 4) & 0xF]);
    putc_(h[v & 0xF]);
}

static volatile uint32_t got;
static volatile uint32_t seen_slot[PEERTAB_MAX];
static uint32_t last_ms[PEERTAB_MAX];   /* ms we last heard each peer */
static int had_slot[PEERTAB_MAX];        /* were we holding them a moment ago? */

void enet_handler(void)
{
    uint32_t st = DMA_STATUS;

    if (st & STAT_RI) {
        got = 1;
    }
    DMA_STATUS = st & STAT_RI;
}

static uint32_t tx_seq;

/* This node's per-boot incarnation, drawn ONCE at boot and stamped into every frame. */
static uint32_t my_incarn;

/*
 * Draw the per-boot incarnation.
 *
 * ⭐ A NONCE THAT IS THE SAME EVERY BOOT IS A CONSTANT WEARING A NONCE'S NAME, AND NO
 *   SINGLE-BOOT TEST CAN SEE THE DIFFERENCE.                             (rt1180 / 95)
 *
 * The honest source is the ELS DTRNG: it reseeds from host entropy at every reset, so two
 * boots of the same firmware draw DIFFERENT words (and two instances on one wire draw
 * different words too).  That is the whole load-bearing property -- proven by the reboot
 * test, and DELIBERATELY BROKEN by -DINCARN_CONSTANT below to show the bug returns without it.
 */
static uint32_t read_incarnation(void)
{
#ifdef INCARN_CONSTANT
    /*
     * NEGATIVE CONTROL (95emulator's case C / rt1180's constant-nonce mutation): a fixed
     * value, so a rebooted peer carries the SAME incarnation as before and its seq reset
     * becomes indistinguishable from a replay again.  A test that cannot reproduce the bug
     * with the fix disabled has not proven the fix is load-bearing.
     */
    return 0xC0FFEE00u;
#else
    uint32_t v = ELS_PRNG_DATOUT;

    /* TX xors any real nonce off the sentinel: a v2 node must NEVER emit 0x5A5A5A5A (that
     * value MEANS "no incarnation"), nor 0 ("refuse to beacon if none"). */
    if (v == BEACON_SENTINEL || v == 0) {
        v ^= 0xA5A5A5A5u;
    }
    return v;
#endif
}

/*
 * BEACON_LONG arms this node to send a frame whose first 64 bytes are a FLAWLESS beacon
 * -- right magic, right self-ethertype, right fill, fresh sequence -- inside a LONGER
 * frame.  Every content check passes.  Only the LENGTH clause can see it.
 *
 * This is rt1180's 1000-byte frame, and it is the impostor 95emulator admitted would have
 * sailed into their peer set: "I read the source for the four fields I thought to look up,
 * and used my instincts for the fifth."
 */
#ifdef BEACON_LONG
#define TX_LEN 128u
#else
#define TX_LEN FRAME_LEN
#endif

static void arm_tx(void)
{
    /*
     * BEACON_REPLAY arms this node to LIE by re-sending a FROZEN sequence number --
     * every frame perfectly well-formed (right magic, self-consistent ethertype,
     * intact pattern) and STALE.  It is exactly what an RX writeback that never lands
     * looks like to a peer, and it is the ONLY thing the freshness check can see.
     *
     * The impostor is a BUILD FLAG rather than a separate source file on purpose: the
     * liar and the honest node are the SAME PROGRAM, so a negative test cannot pass by
     * testing something else.
     */
#ifndef BEACON_REPLAY
    /* A fresh, monotonically increasing sequence in every frame we put on the wire. */
    tx_seq++;
#else
    tx_seq = 1;                 /* FROZEN: every frame is a replay of the first */
#endif
    MEM8(TXBUF + 20) = (tx_seq >> 24) & 0xFF;
    MEM8(TXBUF + 21) = (tx_seq >> 16) & 0xFF;
    MEM8(TXBUF + 22) = (tx_seq >> 8) & 0xFF;
    MEM8(TXBUF + 23) = tx_seq & 0xFF;

    MEM32(TXDESC + 8) = TDES2_IOC | TX_LEN;
    MEM32(TXDESC + 12) = TDES3_OWN | TDES3_FD | TDES3_LD | TX_LEN;
    TXDESC_TAIL = TXDESC + 16;
}

/*
 * Is this frame INTACT, or merely PRESENT?
 *
 *   magic     -- the buffer holds a beacon at all (a DMA to address 0, or a stale
 *                buffer never written back, fails here)
 *   self-et   -- the body's copy of the sender's ethertype must EQUAL the header's.
 *                A FRAME THAT DISAGREES WITH ITSELF IS A CLOBBERED BUFFER: the header
 *                came from one frame and the body from another (or from nothing).
 *   fill      -- the known pattern is the payload's own checksum, cheaply.
 *
 * The sequence number is deliberately NOT checked here: a gap is a LOSS, and a mcast
 * socket may legitimately drop.  CORRUPTION IS THE ASSERTION; LOSS IS A STATISTIC.
 */
/*
 * Why a frame was rejected.  The KIND goes after holobench's ratified
 * "ENET-LAB3 CORRUPT:" token, never in a token of its own -- a brand-new field is the
 * one place where token drift is still free to prevent (91emulator).
 */
#define BAD_OK       0
#define BAD_MAGIC    1
#define BAD_SELF_ET  2
#define BAD_PATTERN  3
#define BAD_REPLAY   4
#define BAD_LEN      5
/*
 * BAD_REBOOT is NOT a corruption -- it is a FRESH sighting of a peer that RESTARTED.  Its
 * seq went backwards (reset to 1), which on seq alone is a replay; only the changed
 * incarnation tells them apart.  The caller announces it once and counts the peer LIVE.
 * A distinct kind, never inside the ratified CORRUPT token (a reboot is not a fault).
 */
#define BAD_REBOOT   6
/*
 * BAD_LEGACY is NOT a corruption and NOT a verified sighting.  A peer whose incarnation is
 * the sentinel (a v1 node that carries none) is PRESENT and well-formed, but its freshness
 * is UNVERIFIABLE -- I cannot tell its reboot from its replay.  So it must NOT gate PASS.
 *
 * ⭐ A LEGACY *REQUIRED* PEER THAT CLOSES THE PASS GATE IS A GREEN THAT MEANS A NODE
 *   QUIETLY STAYED ON THE OLD BODY.  (rt1180emulator, retracted-and-fixed by 95emulator.)
 *   Every configured peer in this lab is REQUIRED, so a legacy one HOLDS THE SEGMENT RED
 *   until it carries a real incarnation.  That red is the cutover's forcing function, not
 *   a false negative.
 */
#define BAD_LEGACY   7

/* Highest sequence number seen from each peer.  A beacon's seq only ever goes UP --
 * WITHIN A BOOT.  Across a reboot it resets, and incarn_slot[] is how we know that. */
static uint32_t seq_slot[PEERTAB_MAX];
static int have_slot[PEERTAB_MAX];
static uint32_t incarn_slot[PEERTAB_MAX];  /* the per-boot nonce we last saw from each peer */
static int have_incarn[PEERTAB_MAX];
static uint32_t gaps;
static uint32_t reboots;
static uint32_t legacy_frames;

/*
 * ⭐ I BUILT A CHECKER THAT ASKS "IS THIS FRAME VALID." A STALE FRAME IS VALID.
 *   THE QUESTION WAS ALWAYS "IS THIS FRAME *NEW*."                    (91emulator)
 *
 * The three well-formedness checks below -- magic, self-consistent ethertype, fill
 * pattern -- are exactly the ones I shipped, and A STALE BUFFER PASSES ALL THREE.
 * When an RX writeback never lands (rt1180's NETC DMA'ing frames to guest physical
 * address ZERO -- 88 of them, in precisely the join-late case this lab exists to
 * create), the buffer still holds THE PREVIOUS GOOD FRAME FROM THAT SAME PEER.  Its
 * magic is right.  Its embedded ethertype agrees with its header PERFECTLY, because
 * both came from the same honest frame.  Its pattern is intact.
 *
 *   ⇒ MY CHECKER COULD NOT SEE THE BUG IT WAS BUILT TO CATCH.  I tested it against a
 *     CORRUPTED frame and never against a STALE one.
 *
 * And the instrument was already in the frame: I have emitted a monotonic sequence
 * number at [20..23] in every beacon since the first commit -- AND NEVER READ IT.
 * I wrote "corruption is the assertion; loss is a statistic" into the spec and then
 * implemented NEITHER half.
 *
 *   ⭐ ASSERT ON A NUMBER GOING UP.
 *       seq <= last  ->  a REPLAYED/STALE buffer.  CORRUPT.  Not a peer sighting.
 *       seq >  last+1 ->  LOSS.  Logged as a statistic, NEVER a failure -- a mcast
 *                         socket may legitimately drop, and a drop is not a corruption.
 */
/* Is this ethertype part of the beacon protocol at all? */
static int is_beacon_et(uint32_t et)
{
    return et == MY_ETHERTYPE || is_peer(et);
}

/*
 * ⭐ A CHECKER IS ONLY AS INDEPENDENT AS ITS *WEAKEST* CLAUSE.            (95emulator)
 *
 * This checker read bytes 12..63 unconditionally and NEVER LOOKED AT THE RECEIVED FRAME
 * LENGTH.  A 1000-byte frame whose first 64 bytes happened to form a valid beacon would
 * have sailed straight through -- and rt1180 was, in fact, putting 1000-byte frames on
 * the wire.  95emulator confessed exactly this hole in their own node ("I read mcx's
 * source for the FOUR fields I thought to look up, and used my instincts for the FIFTH")
 * and their rule found it in mine:
 *
 *   ⭐ A PARTIAL DERIVATION FEELS EXACTLY LIKE A COMPLETE ONE FROM THE INSIDE.
 *     READING THE SOURCE PROTECTS YOU ONLY FOR THE QUESTIONS YOU THOUGHT TO ASK IT.
 *
 * The eQOS RX descriptor writes the packet length back in RDES3[14:0].  The contract says
 * 64 bytes.  Anything else is not this protocol, however convincing its first 64 bytes.
 */
#define RDES3_PL_MASK 0x00007FFFu

static uint32_t rx_len(void)
{
    return MEM32(RXDESC + 12) & RDES3_PL_MASK;
}

static int frame_ok(uint32_t et)
{
    uint32_t magic;
    uint32_t self_et;
    uint32_t seq;
    uint32_t incarn;
    uint32_t *last;
    int *have;
    int slot;
    int i;

    /*
     * ⭐ ASK "IS THIS EVEN MY PROTOCOL?" BEFORE ASKING "IS IT WELL-FORMED?"
     *
     * This used to body-check EVERY non-self frame on the segment.  On a synthetic
     * all-MCX lab that is harmless -- nothing but beacons ever appears.  On a REAL wire
     * with Linux peers, their kernels do multicast NDP/MLD, and we were validating
     * IPv6 (ethertype 0x86DD) against a beacon body it was never going to have, then
     * shouting ENET-LAB3 CORRUPT about it.  holobench's scorer greps that token as a
     * HARD FAIL: this node would have failed the lab because a peer sent a neighbour
     * discovery packet.  (Their 4-node run: "mcx REJECTS 0x86dd x5".)
     *
     *   ⭐ A CORRUPTION DETECTOR THAT CRIES FOUL AT TRAFFIC THAT WAS NEVER ITS PROTOCOL
     *     WILL BE TURNED OFF BY THE PEOPLE IT PROTECTS.
     *
     * AND MY OWN SUITES ARE STRUCTURALLY INCAPABLE OF FINDING THIS.  Every node on my
     * segment is an MCX running this firmware, so no IPv6 -- no ANYTHING but beacons --
     * can ever appear on it.  A suite can be exhaustive within its own model of the
     * world and still be blind BY CONSTRUCTION to everything outside it.  It took a real
     * mixed Linux + bare-metal wire, which is exactly what the 4-node lab is FOR.
     *
     * Non-beacon traffic is now IGNORED: not counted, not condemned, not printed.
     */
    if (!is_beacon_et(et)) {
        return BAD_OK;
    }

    /* The contract says 64 bytes.  A longer frame is not a beacon with extra on the end;
     * it is a different frame that happens to start like one. */
    if (rx_len() != FRAME_LEN) {
        return BAD_LEN;
    }

    magic = ((uint32_t)MEM8(RXBUF + 14) << 24) | ((uint32_t)MEM8(RXBUF + 15) << 16) |
            ((uint32_t)MEM8(RXBUF + 16) << 8)  |  (uint32_t)MEM8(RXBUF + 17);
    if (magic != BEACON_MAGIC) {
        return BAD_MAGIC;
    }

    self_et = ((uint32_t)MEM8(RXBUF + 18) << 8) | (uint32_t)MEM8(RXBUF + 19);
    if (self_et != et) {
        return BAD_SELF_ET;     /* the frame contradicts itself */
    }

    /* Fill is now [28..63]; [24..27] carries the incarnation, which is NOT fill-checked. */
    for (i = 28; i < FRAME_LEN; i++) {
        if (MEM8(RXBUF + i) != BEACON_FILL) {
            return BAD_PATTERN;
        }
    }

    /* FRESHNESS.  Everything above says the frame is WELL-FORMED.  Only this says NEW. */
    seq = ((uint32_t)MEM8(RXBUF + 20) << 24) | ((uint32_t)MEM8(RXBUF + 21) << 16) |
          ((uint32_t)MEM8(RXBUF + 22) << 8)  |  (uint32_t)MEM8(RXBUF + 23);
    incarn = ((uint32_t)MEM8(RXBUF + 24) << 24) | ((uint32_t)MEM8(RXBUF + 25) << 16) |
             ((uint32_t)MEM8(RXBUF + 26) << 8)  |  (uint32_t)MEM8(RXBUF + 27);

    slot = peer_idx(et);
    if (slot < 0) {
        return BAD_OK;          /* our own ethertype; well-formed is all we can say */
    }
    last = &seq_slot[slot];
    have = &have_slot[slot];

    /*
     * ⭐ A LEGACY REQUIRED PEER MUST HOLD THE SEGMENT RED -- IT DOES NOT GET COUNTED AS SEEN.
     *   (Fleet consensus REQUIRED-vs-OBSERVED, from rt1180emulator; 95emulator shipped the
     *   "counted-as-seen, freshness-unverifiable" version and RETRACTED it as the masking
     *   green.  I had adopted the retracted position; 95 caught it here.)
     *
     * A v1 node leaves [24..27] as fill, so incarn reads as the sentinel.  Without a
     * per-boot nonce I cannot distinguish its reboot (seq reset) from its replay (seq
     * frozen) -- so I render NO freshness verdict.  But "no verdict" must NOT mean "counts
     * toward PASS": every configured peer in this lab is REQUIRED, and a required peer whose
     * freshness I never verified CLOSING THE GATE is a green that means a node quietly stayed
     * on the old body.  So a sentinel frame is well-formed and PRESENT (tracked for liveness,
     * announced once) but returns BAD_LEGACY, and the caller does NOT set seen_slot[] on it.
     * The segment stays RED until the peer carries a real incarnation -- the cutover's
     * forcing function.  Exercised by the -DBEACON_LEGACY build; guarded by mcxn-enet-reboot
     * case D (a legacy required peer must NOT let the node PASS).
     */
    if (incarn == BEACON_SENTINEL) {
        legacy_frames++;
        return BAD_LEGACY;      /* present, freshness UNVERIFIABLE -- must NOT gate PASS */
    }

    /*
     * ⭐ ACROSS A REBOOT, seq RESETS -- AND THAT IS NOT A REPLAY.  The incarnation is the
     *   only thing that says so.  A new nonce from a known peer means it RESTARTED: accept
     *   its reset sequence as a fresh baseline instead of condemning it.
     */
    if (!have_incarn[slot]) {
        incarn_slot[slot] = incarn;
        have_incarn[slot] = 1;
    } else if (incarn != incarn_slot[slot]) {
        incarn_slot[slot] = incarn;
        *last = seq;            /* rebaseline onto the new boot's sequence */
        *have = 1;
        reboots++;
        return BAD_REBOOT;      /* a FRESH sighting, announced by the caller -- not a fault */
    }

    if (*have && seq <= *last) {
        /*
         * Same incarnation, sequence did not advance: this did not come off the wire, it
         * came out of a buffer the receiver never rewrote.  DO NOT update *last: a stale
         * frame must not drag our baseline BACKWARDS with it.
         */
        return BAD_REPLAY;
    }
    if (*have && seq > *last + 1) {
        gaps++;                 /* LOSS: a statistic, never a failure */
    }
    *last = seq;
    *have = 1;
    return BAD_OK;
}

static void rearm_rx(void)
{
    MEM32(RXDESC + 12) = RDES3_OWN | RDES3_IOC | RDES3_BUF1V;
    RXDESC_TAIL = RXDESC + 16;
}

/*
 * ⭐ A DEPARTURE MUST BE A POSITIVE ASSERTION, NOT A SILENCE.
 *
 * This node re-armed after every PASS (good: its oracle cannot EXPIRE, unlike a latch
 * that passes once and goes blind).  But when a peer DEPARTED, all it did was STOP
 * PRINTING -- and I had spent the evening telling holobench that inferring health from
 * ABSENCE OF OUTPUT is the silent-watcher trap.  Then I shipped absence of output.
 *
 *   "no verdict" and "still working" are the same observation        (ollama)
 *   a watcher whose absence is silent is not a watcher               (holobench)
 *
 * So the falling edge is now NAMED.  We hold each peer for PEER_HOLD consecutive
 * scans; when one goes quiet we print ENET-LAB3 LOST: <ethertype>, ONCE, at the edge.
 * holobench's scorer can then assert `heartbeat_gap ~= departure_window` on a NUMBER
 * it was TOLD, instead of on a message that failed to arrive.
 */
/*
 * ⭐ A LIVENESS TIMEOUT MUST BE MEASURED IN TIME, NOT IN LOOP ITERATIONS.
 *
 * The hold used to be a SCAN COUNT, and there was NO GOOD VALUE for it:
 *
 *      PEER_HOLD = 20000  ->  took >20 s to fire.  My first departure test printed
 *                             NOTHING and I nearly shipped a falling edge that never
 *                             falls.  (AN ASSERTION THAT HAS NEVER FIRED IS NOT AN
 *                             ASSERTION.)
 *      PEER_HOLD =   300  ->  FLAPPED.  It declared a peer LOST that was ALIVE -- 75
 *                             times on one node -- because normal frame-arrival jitter
 *                             exceeded the hold.
 *
 * ⚠ AND THE SHORT ONE IS THE DANGEROUS DIRECTION.  A liveness timeout that is too
 *   short DOES NOT FAIL SAFE: IT MANUFACTURES DEPARTURES THAT NEVER HAPPENED.  A lab
 *   scoring on those would report a wire failure that did not occur -- a confident,
 *   plausible, WRONG answer, which is the exact bug class this whole tree exists to
 *   refuse.  A FALSE 'LOST' IS WORSE THAN NO 'LOST'.
 *
 * The scan count was never the tuning problem; it WAS the problem.  A scan's duration
 * depends on host load AND on how many frames happen to arrive, so the same constant
 * means a different timeout on every run and every box.
 *
 * So the node now keeps REAL TIME, off SysTick, and the hold is a DURATION.
 * (Assumption, stated: SysTick runs at the SoC's 150 MHz core clock.  The clock tree
 * is not modelled -- see CLAUDE.md -- so this is a documented assumption, not a
 * measurement.  It is exact enough for a one-second liveness window and it is stable
 * across host load, which the spin count never was.)
 */
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SYST_CSR_ENABLE    (1u << 0)
#define SYST_CSR_CLKSOURCE (1u << 2)   /* processor clock */
#define SYST_CSR_COUNTFLAG (1u << 16)  /* RO, clears on read */

#define CORE_HZ   150000000u
#define TICK_MS   1u
#define SYST_RELOAD ((CORE_HZ / 1000u) * TICK_MS - 1u)

/* Milliseconds since boot.  Advanced by polling SysTick's COUNTFLAG. */
static uint32_t now_ms;

static void clock_init(void)
{
    SYST_RVR = SYST_RELOAD;
    SYST_CVR = 0;
    SYST_CSR = SYST_CSR_ENABLE | SYST_CSR_CLKSOURCE;
}

static void clock_poll(void)
{
    /* COUNTFLAG sets on each wrap and CLEARS ON READ -- so no tick is double-counted
     * and none is missed, provided we poll faster than the reload period. */
    if (SYST_CSR & SYST_CSR_COUNTFLAG) {
        now_ms += TICK_MS;
    }
}

#define BEACON_MS 20u          /* broadcast every 20 ms -- a DURATION, not a spin count */
#define PASS_EVERY 200u        /* heartbeat, not firehose: 1 line per 200 re-earnings */
#define PEER_HOLD_MS 1000u      /* a peer unheard for 1 s has departed */

static uint32_t self_frames;
static uint32_t corrupt_frames;
static uint32_t passes;

void cpu0_main(void)
{
    int i;
    volatile int d;
    uint32_t last_tx = 0;             /* ms at which we last beaconed */


    LP_CTRL = CTRL_TE;
    puts_("ENET-LAB3 up: broadcasting ethertype 0x");
    puthex2((MY_ETHERTYPE >> 8) & 0xFF);
    puthex2(MY_ETHERTYPE & 0xFF);
    puts_(", waiting for BOTH peers\r\n");

    /* Draw this boot's incarnation ONCE, before any frame is built.  Printed so a human
     * (and the reboot test) can SEE it change across boots -- a constant here would be the
     * whole bug. */
    my_incarn = read_incarnation();
    puts_("ENET-LAB3 incarnation 0x");
    puthex2((my_incarn >> 24) & 0xFF); puthex2((my_incarn >> 16) & 0xFF);
    puthex2((my_incarn >> 8) & 0xFF);  puthex2(my_incarn & 0xFF);
    puts_("\r\n");

    DMA_MODE = DMA_SWR;
    while (DMA_MODE & DMA_SWR) {
    }

    /* A broadcast frame carrying this node's EtherType. */
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + i) = 0xFF;                 /* dest = broadcast */
    }
    for (i = 0; i < 6; i++) {
        MEM8(TXBUF + 6 + i) = MY_MAC[i];        /* src */
    }
    /*
     * BEACON_NOISE arms this node to impersonate a LINUX PEER: it emits ethertype
     * 0x86DD (IPv6) with a body that is NOT a beacon body -- which is exactly what a
     * real Linux node's kernel puts on a shared segment (multicast NDP/MLD).
     *
     * It exists because my beacon-only lab is STRUCTURALLY INCAPABLE of producing
     * foreign traffic: every node on it speaks this protocol.  holobench had to run a
     * REAL mixed Linux + bare-metal wire to discover that my detector was shouting
     * ENET-LAB3 CORRUPT at IPv6.  A fix I cannot test is a fix I am merely ASSERTING,
     * so the noise is now something my own segment can carry.
     */
#ifdef BEACON_NOISE
    MEM8(TXBUF + 12) = 0x86;    /* IPv6 -- NOT a beacon ethertype */
    MEM8(TXBUF + 13) = 0xDD;
    for (i = 14; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = (uint8_t)(0x60 + i);   /* not a beacon body, and never will be */
    }
#else
    MEM8(TXBUF + 12) = (MY_ETHERTYPE >> 8) & 0xFF;
    MEM8(TXBUF + 13) = MY_ETHERTYPE & 0xFF;

    /*
     * ⭐ THE BODY IS THE EVIDENCE.  THE ETHERTYPE IS ONLY THE ROUTING KEY.
     *
     * This lab used to count a peer sighting on the ETHERTYPE ALONE -- and
     * "I saw 0x88B6" is a statement about a FIELD, not about a FRAME.  rt1180
     * found its NETC DMA'ing whole frames to GUEST PHYSICAL ADDRESS ZERO (88 of
     * them, in exactly the join-late case this lab exists to create), and a frame
     * whose body is garbage carries THE SAME ETHERTYPE AS A GOOD ONE.  Every one
     * of those corrupt frames would have been counted here as a healthy peer, and
     * this node would have printed PASS.
     *
     * holobench: "five transports prove the DATA crossed; the sixth -- the only one
     * whose purpose is to FIND BUGS -- proves a NUMBER ARRIVED."  So:
     *
     *   [14..17]  magic 0xB5B6B7C0
     *   [18..19]  the sender's OWN ethertype, repeated in the body.  A frame that
     *             DISAGREES WITH ITSELF is a stale or clobbered buffer -- which is
     *             precisely what a write-back bug produces.
     *   [20..23]  monotonic sequence, per sender
     *   [24..27]  per-boot incarnation -- tells a peer's REBOOT (new nonce, seq reset)
     *             from a REPLAY (same nonce, seq frozen); 0x5A5A5A5A == legacy/none
     *   [28..63]  0x5A, the same known byte the SPI/I2C labs already assert on
     *
     * CORRUPTION IS THE ASSERTION; LOSS IS A STATISTIC.  A gap in the sequence is
     * logged, never failed -- a mcast socket may legitimately drop a frame, and a
     * drop is not a corruption.
     */
    MEM8(TXBUF + 14) = (BEACON_MAGIC >> 24) & 0xFF;
    MEM8(TXBUF + 15) = (BEACON_MAGIC >> 16) & 0xFF;
    MEM8(TXBUF + 16) = (BEACON_MAGIC >> 8) & 0xFF;
    MEM8(TXBUF + 17) = BEACON_MAGIC & 0xFF;
    MEM8(TXBUF + 18) = (MY_ETHERTYPE >> 8) & 0xFF;   /* must equal bytes 12..13 */
    MEM8(TXBUF + 19) = MY_ETHERTYPE & 0xFF;
#ifdef BEACON_LEGACY
    /*
     * LEGACY (v1) EMITTER: leave [24..27] as fill, so a receiver reads the 0x5A5A5A5A
     * sentinel and knows this node carries NO incarnation.  Exists to exercise the
     * receiver's freshness-unverifiable path -- otherwise that path is untested code.
     */
    for (i = 24; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = BEACON_FILL;
    }
#else
    /* v2: the per-boot incarnation at [24..27] (BE), then fill [28..63]. */
    MEM8(TXBUF + 24) = (my_incarn >> 24) & 0xFF;
    MEM8(TXBUF + 25) = (my_incarn >> 16) & 0xFF;
    MEM8(TXBUF + 26) = (my_incarn >> 8) & 0xFF;
    MEM8(TXBUF + 27) = my_incarn & 0xFF;
    for (i = 28; i < FRAME_LEN; i++) {
        MEM8(TXBUF + i) = BEACON_FILL;
    }
#endif
#endif
    MEM32(TXDESC + 0) = TXBUF;
    MEM32(TXDESC + 4) = 0;

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

    peers_init();
    clock_init();
    rearm_rx();

    /* Retry forever: a Linux peer may take minutes to bring its iface up. */
    /*
     * ⭐ THE NODE MUST CONSUME FASTER THAN THE SEGMENT PRODUCES, OR IT IS WATCHING
     *   ITS OWN BACKLOG.
     *
     * This loop used to TRANSMIT ONCE PER ITERATION and CONSUME AT MOST ONE FRAME PER
     * ITERATION -- with TWO peers each doing the same, frames arrived TWICE as fast as
     * they could be drained.  The backlog grew without bound.  So when a peer was
     * KILLED, this node went on processing that peer's OLD frames for many seconds,
     * each one REFRESHING its liveness timer, AND THE DEPARTURE COULD NEVER BE SEEN.
     *
     * Three departure tests printed no LOST line.  The falling-edge logic was correct
     * in all three.  The node was simply too far behind to know what time it was.
     *
     * So: the beacon is now RATE-LIMITED BY THE CLOCK (BEACON_MS), and RX drains FLAT
     * OUT with no spin delay at all.  Consumption now exceeds production and the
     * backlog cannot accumulate.
     *
     * ⇒ This also retires the confession I owed holobench: the beacon interval WAS a
     *   bare `for (d = 0; d < 60000; d++)` spin, whose period drifted with host load.
     *   IT IS NOW A DURATION.  A delay loop in a test is a confession; this one is
     *   paid off.
     */
    for (i = 0; ; i++) {
        clock_poll();

        if ((now_ms - last_tx) >= BEACON_MS) {
            arm_tx();
            last_tx = now_ms;
        }

        if (got) {
            uint32_t et = ((uint32_t)MEM8(RXBUF + 12) << 8) | MEM8(RXBUF + 13);
            int mine;
            int bad;

            got = 0;

            /*
             * ⚠ BOTH SELF-DOORS.  THE HEADER COMMENT ABOVE CLAIMED WE IGNORED FRAMES
             *   CARRYING "our own EtherType OR SOURCE MAC".  THE ETHERTYPE DOOR WAS
             *   REAL.  THE MAC DOOR WAS NEVER WRITTEN -- only its documentation was.
             *
             *   ⭐ A COMMENT THAT CLAIMS A CHECK THE CODE DOES NOT MAKE IS WORSE THAN
             *     NO COMMENT: IT DISCHARGES THE REVIEWER'S SUSPICION WITHOUT
             *     DISCHARGING THE RISK.  I read that line twice tonight and it
             *     answered a question I never went and asked the code.
             *
             *   And it matters, because the mcast socket hands our own broadcast
             *   straight back, continuously -- 91emulator measures ~235 self-frames
             *   per node per run.  With ONLY the ethertype door, a node whose
             *   ethertype is mis-set (a build-time -D, easy to get wrong) COUNTS
             *   ITSELF AS ITS OWN PEER AND PASSES ALONE ON AN EMPTY WIRE.
             *
             *   The MAC door is independent of the ethertype, so it survives exactly
             *   the misconfiguration that opens the first one.
             */
            mine = (et == MY_ETHERTYPE);
            for (d = 0; d < 6; d++) {
                if (MEM8(RXBUF + 6 + d) != MY_MAC[d]) {
                    break;
                }
            }
            if (d == 6) {
                mine = 1;          /* our own source MAC: this frame is our echo */
                self_frames++;
            }

            if (!mine) {
                /*
                 * ⚠ WE USED TO PRINT EVERY FRAME WE RECEIVED, AND IT MADE THIS NODE
                 *   UNABLE TO WATCH ITS OWN SEGMENT.
                 *
                 *   At ~9000 serial lines/second the print was FAR slower than the
                 *   wire, so the node fell permanently behind and processed a growing
                 *   socket BACKLOG.  Its consequence is the one that matters here:
                 *   after a peer was KILLED, its queued frames kept arriving for many
                 *   seconds -- each one RESETTING that peer's liveness timer -- so the
                 *   departure could NEVER be detected.  My first two departure tests
                 *   printed no LOST line at all, and the falling-edge logic was fine.
                 *
                 *   ⭐ AN OBSERVER THAT CANNOT KEEP UP WITH ITS SUBJECT IS NOT
                 *     OBSERVING THE SUBJECT -- IT IS OBSERVING ITS OWN BACKLOG.
                 *     The instrument was loading the experiment, and "the peer is
                 *     still here" and "I am 40,000 frames behind" were the same
                 *     observation.
                 *
                 * So: announce each peer ONCE, on first sighting.  The greppable
                 * tokens holobench's scorer actually keys on -- PASS / CORRUPT / LOST
                 * -- are all rare, and they stay.
                 */
                if (peer_idx(et) >= 0 && !had_slot[peer_idx(et)]) {
                    puts_("ENET-LAB3 rx: ethertype 0x");
                    puthex2((et >> 8) & 0xFF); puthex2(et & 0xFF);
                    puts_(" src ");
                    for (d = 0; d < 6; d++) {
                        puthex2(MEM8(RXBUF + 6 + d));
                        if (d < 5) {
                            putc_(':');
                        }
                    }
                    puts_("\r\n");
                }

                /*
                 * ⭐ VERIFY THE FRAME, NOT THE FIELD.  A corrupt frame has the same
                 *   ethertype as a good one, so counting on the ethertype alone makes
                 *   "delivered" and "delivered corrupt" THE SAME OBSERVATION -- and a
                 *   lab whose whole purpose is to find corruption cannot afford that.
                 */
                bad = frame_ok(et);
                if (bad == BAD_REBOOT) {
                    /*
                     * A peer's per-boot incarnation CHANGED: it restarted.  Its seq reset,
                     * which on seq alone is a replay -- the incarnation is what tells them
                     * apart.  This is a LIVE, FRESH peer, so announce it once (a distinct
                     * kind, NOT inside the CORRUPT token) and fall through to count it seen.
                     */
                    puts_("ENET-LAB3 REBOOT: peer 0x");
                    puthex2((et >> 8) & 0xFF); puthex2(et & 0xFF);
                    puts_(" new incarnation 0x");
                    for (d = 24; d < 28; d++) {
                        puthex2(MEM8(RXBUF + d));
                    }
                    puts_("\r\n");
                    bad = BAD_OK;
                }
                if (bad == BAD_LEGACY) {
                    /*
                     * PRESENT, BUT ON THE OLD BODY.  Track liveness (it is here) and
                     * announce it ONCE -- but do NOT set seen_slot[]: a legacy required
                     * peer must HOLD THE PASS GATE RED until it carries a real incarnation.
                     * Not corrupt either -- a v1 frame is well-formed, just unverifiable.
                     */
                    int sl = peer_idx(et);

                    if (sl >= 0) {
                        last_ms[sl] = now_ms;
                        if (!had_slot[sl]) {
                            puts_("ENET-LAB3 LEGACY: peer 0x");
                            puthex2((et >> 8) & 0xFF); puthex2(et & 0xFF);
                            puts_(" no incarnation -- present, freshness UNVERIFIABLE, "
                                  "holding PASS RED\r\n");
                        }
                        had_slot[sl] = 1;   /* present (gates LOST); NOT seen (gates PASS) */
                    }
                } else if (bad) {
                    /*
                     * holobench ratified "ENET-LAB3 CORRUPT:" as THE bad-frame token.
                     * The KIND goes AFTER it, where free-form is welcome -- a new token
                     * would be the drift bug in the one field where it is still free to
                     * prevent.  A scorer grepping the ratified prefix catches replays
                     * with no change at all.
                     */
                    corrupt_frames++;
                    puts_("ENET-LAB3 CORRUPT: ");
                    if (bad == BAD_REPLAY) {
                        puts_("PAYLOAD-REPLAY");
                    } else if (bad == BAD_MAGIC) {
                        puts_("BAD-MAGIC");
                    } else if (bad == BAD_SELF_ET) {
                        puts_("SELF-ET-MISMATCH");
                    } else if (bad == BAD_LEN) {
                        puts_("BAD-LENGTH");
                    } else {
                        puts_("BAD-PATTERN");
                    }
                    puts_(" et=0x");
                    puthex2((et >> 8) & 0xFF); puthex2(et & 0xFF);
                    puts_(" magic=0x");
                    for (d = 14; d < 18; d++) {
                        puthex2(MEM8(RXBUF + d));
                    }
                    puts_(" self-et=0x");
                    puthex2(MEM8(RXBUF + 18)); puthex2(MEM8(RXBUF + 19));
                    puts_(" seq=0x");
                    for (d = 20; d < 24; d++) {
                        puthex2(MEM8(RXBUF + d));
                    }
                    puts_("\r\n");
                } else {
                    {
                        int sl = peer_idx(et);

                        if (sl >= 0) {
                            seen_slot[sl] = 1;
                            last_ms[sl] = now_ms;   /* fresh evidence, timestamped */
                            had_slot[sl] = 1;
                        }
                    }
                }

                /*
                 * PASS requires EVERY CONFIGURED peer.  With PEER_C absent (0) this is
                 * exactly the old two-peer condition, so the 3-node suites are unchanged;
                 * with PEER_C set, a node that cannot see imx91 does not get to say it
                 * saw the segment.
                 */
                {
                    uint32_t k;
                    int all = peer_n > 0;

                    for (k = 0; k < peer_n; k++) {
                        if (!seen_slot[k]) {
                            all = 0;
                        }
                    }
                    if (all) {
                    /*
                     * Re-earned, never latched -- an assertion that has already passed
                     * cannot fail again, so this one keeps having to be true
                     * (holobench).  But it is a HEARTBEAT, not a firehose: printing it
                     * on every scan is what drowned the node.  One line per PASS_EVERY
                     * re-earnings; the FIRST is immediate so the lab sees it at once.
                     */
                    if ((passes++ % PASS_EVERY) == 0) {
                        /*
                         * The beat carries its own timestamp so holobench's scorer is
                         * TOLD when it happened rather than having to infer it.
                         *
                         * ⚠ BUT READ THE CAVEAT AND BELIEVE IT: `now_ms` is derived
                         *   from SysTick assuming a 150 MHz core, and WITHOUT -icount
                         *   QEMU runs those cycles as fast as the host allows -- so
                         *   this counter ran ~3.7x FAST when I measured it.  It is
                         *   MONOTONIC and it is fine for a liveness timeout.  IT IS
                         *   NOT WALL-CLOCK MILLISECONDS.
                         *
                         *   ⇒ holobench: use it for ORDERING and for the EVENT.  Take
                         *     your DURATIONS from 91emulator's node, which reads a real
                         *     clock (gettimeofday over the ARM generic timer).  A number
                         *     I cannot defend is worse than a number I do not offer.
                         */
                        puts_("ENET-LAB3 PASS: saw BOTH peers on the segment t=");
                        puthex2((now_ms >> 24) & 0xFF); puthex2((now_ms >> 16) & 0xFF);
                        puthex2((now_ms >> 8) & 0xFF);  puthex2(now_ms & 0xFF);
                        puts_("\r\n");
                    }
                    for (k = 0; k < peer_n; k++) {
                        seen_slot[k] = 0;      /* re-arm; keep the segment alive */
                    }
                    }
                }
            }
            rearm_rx();
        }

        /*
         * Age each peer.  A peer we WERE holding and have not heard from in PEER_HOLD
         * scans has DEPARTED -- and we say so, once, out loud.
         */
        {
            uint32_t k;

            for (k = 0; k < peer_n; k++) {
                if (had_slot[k] && (now_ms - last_ms[k]) > PEER_HOLD_MS) {
                    puts_("ENET-LAB3 LOST: peer 0x");
                    puthex2((peer_et[k] >> 8) & 0xFF);
                    puthex2(peer_et[k] & 0xFF);
                    puts_(" went quiet\r\n");
                    had_slot[k] = 0;
                    seen_slot[k] = 0;
                }
            }
        }
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[160] = {
    [0]  = (vec_t)0x20010000u,  /* initial MSP */
    [1]  = cpu0_main,           /* Reset_Handler */
    [16 + ENET_IRQ] = enet_handler,
};
