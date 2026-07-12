/*
 * MCXN947 FlexCAN receive path — ID matching and a HONEST overrun.
 *
 * Found by sweeping for "gates that can never say no" (ollama_95_neutron's
 * no-op safety check): flexcan_bus_can_receive() returned true unconditionally.
 * That was the symptom.  Behind it were three real bugs, all of the kind a CAN
 * node in a board farm could never diagnose from inside firmware:
 *
 *   1. NO ID MATCHING on the bus path.  A frame was dropped into the first
 *      CODE=EMPTY mailbox, whatever ID that mailbox had been configured for.  A
 *      driver that trusts its own filter — "MB5 is my ID, so whatever lands in
 *      MB5 is mine" — would read SOMEBODY ELSE'S FRAME and never know.  (The
 *      loopback path did filter; the BOARD-TO-BOARD path, the one that matters,
 *      did not.  The two had silently diverged.)
 *
 *   2. A FULL MAILBOX MEANT A SILENTLY DROPPED FRAME.  The code scanned only for
 *      CODE=EMPTY and, finding none, returned SUCCESS having thrown the frame
 *      away.  Its own comment admitted "a real device flags overrun".  Per the
 *      RM the matching process considers EMPTY, FULL *and* OVERRUN buffers: a
 *      frame landing on an unserviced mailbox OVERWRITES it and sets
 *      CODE=OVERRUN (0110b).  The data still moves AND THE GUEST IS TOLD.
 *      Dropping in silence is the worst of both.
 *
 *   3. A DISABLED CONTROLLER STILL RECEIVED.  MCR[MDIS] was never checked, so a
 *      FlexCAN the guest had switched off went on quietly filling its mailboxes
 *      with traffic the silicon would never have delivered.
 *
 * Exercised through CTRL1[LPB] loopback, so no peer is needed: a transmitted
 * frame is matched against the receive mailboxes exactly as a bus frame is.
 *
 * Prints "CANRX PASS" only if every check holds.
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


/*
 * CAN0 transmits onto a REAL can-bus; CAN1 receives off it.  Both controllers
 * sit on one bus (-machine canbus0=cb,canbus1=cb), so the frame travels the
 * BOARD-TO-BOARD path — flexcan_send_to_bus() -> can-bus -> flexcan_bus_receive()
 * — which is exactly where the bugs were.
 *
 * The first version of this test used CTRL1[LPB] loopback instead, and MUTATION
 * TESTING CAUGHT IT: disabling ID matching in the bus receive path left the test
 * GREEN, because loopback delivery is a SEPARATE code path with its own matching.
 * The test could not fail in the dimension it existed to protect.  A test that
 * exercises the wrong path is decoration, and you cannot tell by reading it.
 */
#define CAN0 0x400D4000u        /* transmitter */
#define CAN1 0x400D8000u        /* receiver    */

#define TX_MCR       (*(volatile uint32_t *)(CAN0 + 0x000))
#define TX_MB(n)     (CAN0 + 0x080u + (n) * 0x10u)
#define TX_CS(n)     (*(volatile uint32_t *)(TX_MB(n) + 0x0))
#define TX_ID(n)     (*(volatile uint32_t *)(TX_MB(n) + 0x4))
#define TX_D0(n)     (*(volatile uint32_t *)(TX_MB(n) + 0x8))

#define CAN_MCR      (*(volatile uint32_t *)(CAN1 + 0x000))
#define CAN_RXMGMASK (*(volatile uint32_t *)(CAN1 + 0x010))
#define CAN_IFLAG1   (*(volatile uint32_t *)(CAN1 + 0x030))
#define MB(n)        (CAN1 + 0x080u + (n) * 0x10u)
#define MB_CS(n)     (*(volatile uint32_t *)(MB(n) + 0x0))
#define MB_ID(n)     (*(volatile uint32_t *)(MB(n) + 0x4))
#define MB_D0(n)     (*(volatile uint32_t *)(MB(n) + 0x8))

#define MCR_MDIS   (1u << 31)
#define MCR_FRZ    (1u << 30)
#define MCR_HALT   (1u << 28)
#define MCR_NOTRDY (1u << 27)
#define CTRL1_LPB  (1u << 12)

#define CODE_SHIFT      24
#define CODE_MASK       0xF
#define CODE_RX_EMPTY   0x4
#define CODE_RX_FULL    0x2
#define CODE_RX_OVERRUN 0x6
#define CODE_TX_DATA    0xC

/* Standard IDs sit left-aligned at bit 18 in the MB ID register. */
#define SID(x) ((uint32_t)(x) << 18)

#define MB_CODE(n) ((MB_CS(n) >> CODE_SHIFT) & CODE_MASK)

/* Arm MB n to receive frames whose ID matches `id`. */
static void rx_arm(int n, uint32_t id)
{
    MB_CS(n) = CODE_RX_EMPTY << CODE_SHIFT;
    MB_ID(n) = id;
    MB_D0(n) = 0;
}

/* CAN0 puts a frame on the real bus; CAN1 must match it. */
static void tx_send(uint32_t id, uint32_t payload)
{
    TX_ID(0) = id;
    TX_D0(0) = payload;
    TX_CS(0) = (CODE_TX_DATA << CODE_SHIFT) | (4u << 16);   /* DLC = 4 */
}

void cpu0_main(void)
{
    int ok = 1;

    LP_CTRL = CTRL_TE;
    puts_("FlexCAN RX test\r\n");

    /* Both controllers on the bus: MDIS=0, HALT=0.  NO loopback — the frame
     * really crosses the can-bus from CAN0 to CAN1. */
    TX_MCR  = 0;
    CAN_MCR = 0;

    /* Match on the full standard-ID field: the mailbox filters really matter. */
    CAN_RXMGMASK = SID(0x7FF);

    /* --- 1: a frame must land in the mailbox that FILTERED FOR IT ---------- */
    rx_arm(4, SID(0x123));                 /* MB4 listens for 0x123 */
    rx_arm(5, SID(0x456));                 /* MB5 listens for 0x456 */

    CAN_IFLAG1 = 0xFFFFFFFFu;              /* W1C */
    tx_send(SID(0x456), 0xAABBCCDDu);      /* ...send 0x456 */

    /* MB4 is EMPTY and comes first.  A model with no ID matching drops the
     * frame there — which is precisely the bug. */
    ok &= (MB_CODE(4) == CODE_RX_EMPTY);   /* MB4 was NOT listening for this */
    ok &= (MB_CODE(5) == CODE_RX_FULL);    /* MB5 was                        */
    ok &= (MB_ID(5) == SID(0x456));
    ok &= (MB_D0(5) == 0xAABBCCDDu);

    /* --- 2: a second frame on the UNSERVICED mailbox must OVERRUN, not vanish */
    tx_send(SID(0x456), 0x11223344u);      /* MB5 still holds the first frame */

    ok &= (MB_CODE(5) == CODE_RX_OVERRUN); /* the guest is TOLD it lost one   */
    ok &= (MB_D0(5) == 0x11223344u);       /* and the NEW frame is the one kept */

    /* Servicing it and taking another frame returns CODE to FULL. */
    rx_arm(5, SID(0x456));
    tx_send(SID(0x456), 0x55667788u);
    ok &= (MB_CODE(5) == CODE_RX_FULL);
    ok &= (MB_D0(5) == 0x55667788u);

    /* --- 3: a DISABLED controller must not receive ------------------------- */
    rx_arm(5, SID(0x456));
    MB_D0(5) = 0xEEEEEEEEu;                /* poison: a receive overwrites it */
    CAN_MCR = MCR_MDIS;                    /* switch the module OFF           */
    tx_send(SID(0x456), 0x99999999u);

    ok &= (MB_CODE(5) == CODE_RX_EMPTY);   /* nothing was delivered           */
    ok &= (MB_D0(5) == 0xEEEEEEEEu);       /* the buffer is untouched         */

    puts_(ok ? "CANRX PASS\r\n" : "CANRX FAIL\r\n");
    for (;;) {
    }
}

typedef void (*vec_t)(void);
__attribute__((section(".vectors"), used))
const vec_t vectors[2] = { (vec_t)0x20010000u, cpu0_main };
