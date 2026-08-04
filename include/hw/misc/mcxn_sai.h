/*
 * NXP MCX N SAI (Serial Audio Interface / I2S) — real FIFO data path.
 *
 * This block used to be a register file that MOVED NO DATA: TDR writes were
 * discarded and RDR reads returned 0, while the capability table advertised a
 * "SAI FIFO data path".  Mutation-testing caught it — replacing RDR with
 * 0xA5A5A5A5 left the test green, because the test only checked that an
 * interrupt fired.  The claim was retracted before this was written; this is
 * the data path being earned back.
 *
 * What firmware can actually observe about a SAI, and what is therefore
 * modelled for real:
 *
 *   - an 8-word transmit FIFO (TCR1[TFW] is a 3-bit watermark, so depth 8);
 *   - words leave it at the WORD RATE derived from the configuration — bit
 *     clock = MCLK / (2 * (TCR2[DIV] + 1)), and a word is TCR5[W0W] + 1 bits
 *     — so the FIFO drains at a rate firmware set, not instantly;
 *   - TCSR[FWF] (watermark), [FRF] (request) and [FEF] (FIFO error) computed
 *     from real occupancy: writing a full FIFO overruns, and running it dry
 *     underruns, both of which real firmware must handle and neither of
 *     which a register file can express;
 *   - the same on the receive side, with RDR popping a real FIFO.
 *
 * The audio pins have nothing attached in emulation, so a transmitted word
 * goes nowhere — as it would on a board with no codec.  The "loopback"
 * property wires SAI_TXD back into SAI_RXD, which is the digital equivalent
 * of jumpering the two pins on the bench, and is exactly how you exercise a
 * SAI without a codec.  It is a BOARD-LEVEL option, not a register: the MCX
 * N SAI has no loopback bit (the RM gives one to LPUART and to FlexCAN, and
 * none to the SAI), and inventing one would be fabricating silicon.
 * Default off; the operator opts in.
 *
 * ✅ The MCLK is now DERIVED (2026-07-21), no longer a hardcoded figure: the
 * SAI takes a Clock input from SYSCON SAI0CLKSEL/CLKDIV, so the absolute
 * rate follows the source firmware selects.  Every RATIO the SAI produces
 * was always MEASURED (MCLK cancels); the DERIVATION itself is now tested
 * too (mcxn-sai's FRO_HF-vs-PLL0 word-rate check).  See the detailed note
 * below the register offsets.
 *
 * Offsets/bits from the MCXN947 CMSIS header (I2S_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_SAI_H
#define HW_MISC_MCXN_SAI_H

#include "hw/core/sysbus.h"
#include "hw/core/clock.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_MCXN_SAI "mcxn-sai"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNSAIState, MCXN_SAI)

#define MCXN_SAI_SIZE      0x1000
#define MCXN_SAI_FIFO_DEPTH 8      /* TCR1[TFW] is 3 bits -> 8 words */

/*
 * ⚠ NOT MEASURED. NOT DERIVED. AN ASSUMPTION — AND LABELLED AS ONE, ON THE
 * NUMBER.
 *
 *   Kyle's LAW 1: "If you truly CANNOT test it, that's allowed -- but you
 *   LABEL it, ON THE NUMBER, IN THE SAME BREATH.  An unlabelled untestable
 *   number is forbidden."
 *
 *   This used to say "Nominal audio master clock".  ⭐ AND "NOMINAL" IS THE
 *   EUPHEMISM, NOT THE ADMISSION.  This tree's own guardrails list it beside
 *   "plausible" and "best-effort" as THE WORDS YOU USE WHEN YOU MEAN
 *   FABRICATED -- so a label written in that word is a label that
 *   DISGUISES.  It reads like a fact and is not one.
 *
 * WHAT IS TRUE, AND WHAT IS NOT:
 *
 *   ✅ MEASURED: every RATIO the SAI produces.  The word period scales
 *      exactly with (TCR2[DIV] + 1) and with TCR5[W0W]+1, and TCR2[BYP]
 *      gives divide-by-one -- and MCLK CANCELS OUT of every one of those.
 *      tests/mcxn-sai measures them: DIV=15 drains in 50016 SysTick ticks,
 *      DIV=15+BYP in 1578.  That is 31.7x against an ideal 32.0 -- and it is
 *      31.7 and NOT 32.000 precisely BECAUSE it was measured.
 *
 *   ✅ RESOLVED (2026-07-21): the ABSOLUTE MCLK is no longer a hardcoded
 *      constant.  The SAI takes a Clock input driven by SYSCON
 *      SAInCLKSEL/CLKDIV (CLOCK_GetSaiClkFreq: PLL0 / ExtClk / FRO_HF /
 *      PLL1-div), so `sai_word_period_ns` reads clock_get_hz(s->clk) and the
 *      rate is DERIVED from whatever source firmware selects -- and follows
 *      a PLL reconfigure.  The classic 12.288 MHz is now a real
 *      CONFIGURATION (PLL1 set to an audio multiple, SAI0CLKSEL=4) rather
 *      than an assumption; a guest that selects FRO_HF gets 48 MHz here,
 *      exactly as on silicon.  No source selected -> 0 Hz -> the SAI has no
 *      MCLK and does not clock data (never a plausible-but-wrong rate).
 *
 *      The old 12.288 MHz assumption is gone.  The RATIO checks (which
 *      cancel MCLK) still hold; and the DERIVATION is now itself testable
 *      -- point the SAI MCLK and SysTick at different known sources and the
 *      word-rate ratio between them is a golden from the SDK's source rates,
 *      not from the model (tests/mcxn-sai).
 */

struct MCXNSAIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    /* SAI function clock (MCLK) — from SYSCON SAInCLKSEL/CLKDIV */
    Clock *clk;
    qemu_irq irq;

    /*
     * DMA request lines into the eDMA (SAI0 Tx = mux source 100, Rx = 99).
     * The FIFO-request condition gated by TCSR/RCSR[FRDE].
     */
    qemu_irq dma_req_tx;
    qemu_irq dma_req_rx;
    bool     tx_dma_req;
    bool     rx_dma_req;
    uint32_t regs[MCXN_SAI_SIZE / 4];

    /*
     * "loopback" property: wire SAI_TXD back to SAI_RXD, as a bench jumper
     * would.  Not a hardware register — the SAI has no loopback bit.
     */
    bool     loopback;

    uint32_t tx_fifo[MCXN_SAI_FIFO_DEPTH];
    uint32_t tx_count;
    uint32_t rx_fifo[MCXN_SAI_FIFO_DEPTH];
    uint32_t rx_count;

    QEMUTimer word_timer;     /* one tick per transmitted word */
    int64_t   next_word_ns;   /* the DEADLINE: a word clock must not drift */
};

#endif /* HW_MISC_MCXN_SAI_H */
