/*
 * NXP MCX N SAI (Serial Audio Interface / I2S) — real FIFO data path.
 * See the header for what is modelled and why the "loopback" property is a
 * board-level jumper rather than an invented register bit.
 *
 * Models the SAI transmit and receive control/status registers (CMSIS
 * I2S_Type) so firmware audio init never hangs:
 *
 *   - TCSR/RCSR software-reset (SR) and FIFO-reset (FR) bits are momentary in
 *     real hardware; here they self-clear so the "set SR, wait for SR clear"
 *     init step terminates immediately.
 *   - The transmit FIFO always reads as having space (FWF warning flag set,
 *     FEF empty flag set), and the receive FIFO reads as empty (RCSR FWF/FEF
 *     reflect "no data, space available") so polling loops resolve.
 *   - The transmit/receive enable bits (TE/RE) read back exactly as written so
 *     "enable then confirm" sequences pass.
 *   - Status flags FRF/FWF/FEF/SEF/WSF are write-1-to-clear.
 *
 * The transmit/receive data and FIFO words are otherwise permissively backed.
 * Offsets and bit masks come from the MCXN947 CMSIS header (I2S_Type).  VERID
 * and PARAM are read-only constants; PARAM's FIFO field is DERIVED from the
 * modelled depth so it cannot drift.  VERID is UNVERIFIED (the RM does not
 * document it) for this SAI revision (firmware does not gate on it).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_sai.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "system/dma.h"
#include "migration/vmstate.h"

/* Register offsets (CMSIS I2S_Type). */
#define SAI_VERID   0x00    /* RO */
#define SAI_PARAM   0x04    /* RO */
#define SAI_TCSR    0x08    /* Transmit Control/Status */
#define SAI_TCR1    0x0C
#define SAI_TCR2    0x10
#define SAI_TCR3    0x14
#define SAI_TCR4    0x18
#define SAI_TCR5    0x1C
#define SAI_TDR0    0x20    /* TDR[2] @0x20..0x24, WO */
#define SAI_TFR0    0x40    /* TFR[2] @0x40..0x44, RO */
#define SAI_TMR     0x60
#define SAI_RCSR    0x88    /* Receive Control/Status */
#define SAI_RCR1    0x8C
#define SAI_RCR2    0x90
#define SAI_RCR3    0x94
#define SAI_RCR4    0x98
#define SAI_RCR5    0x9C
#define SAI_RDR0    0xA0    /* RDR[2] @0xA0..0xA4, RO */
#define SAI_RFR0    0xC0    /* RFR[2] @0xC0..0xC4, RO */
#define SAI_RMR     0xE0
#define SAI_MCR     0x100

/* Configuration fields that set the word rate. */
#define TCR1_TFW(v)  ((v) & 0x7)             /* transmit FIFO watermark   */
#define RCR1_RFW(v)  ((v) & 0x7)             /* receive FIFO watermark    */
#define TCR2_DIV(v)  ((v) & 0xFF)            /* bit-clock divider         */
#define TCR5_W0W(v)  (((v) >> 16) & 0x1F)    /* word width - 1            */

/* TCSR/RCSR bit masks (shared layout for the two CSR registers). */
#define CSR_FRDE    (1u << 0)   /* FIFO request DMA enable  */
#define CSR_FRF     (1u << 16)  /* FIFO request flag        */
#define CSR_FWF     (1u << 17)  /* FIFO warning flag        */
#define CSR_FEF     (1u << 18)  /* FIFO error (underrun/overrun) flag */
#define CSR_SEF     (1u << 19)  /* sync error flag          */
#define CSR_WSF     (1u << 20)  /* word start flag          */
#define CSR_SR      (1u << 24)  /* software reset           */
#define CSR_FR      (1u << 25)  /* FIFO reset               */
#define CSR_BCE     (1u << 28)  /* bit clock enable         */
#define CSR_DBGE    (1u << 29)
#define CSR_STOPE   (1u << 30)
#define CSR_EN      (1u << 31)  /* TE for TCSR / RE for RCSR */

/* W1C flag bits within TCSR/RCSR. */
#define CSR_FLAGS_W1C  (CSR_FEF | CSR_SEF | CSR_WSF)

/*
 * Interrupt-enable bits sit 8 below their status flag (FRIE@8 enables FRF@16,
 * FWIE@9 enables FWF@17, FEIE@10 enables FEF@18, ...).  An interrupt is
 * requested when any (flag & matching-enable) is set.
 */
#define CSR_IE_TO_FLAG_SHIFT  8
#define CSR_STICKY_FLAGS  (CSR_FEF | CSR_SEF | CSR_WSF)

/*
 * ⚠ THESE WERE LABELLED "best-effort".  That word, and its friends
 *   "plausible" and "nominal", are in this tree's guardrails as THE WORDS YOU
 *   USE WHEN YOU MEAN FABRICATED -- and I had written all three, in three
 *   different files.
 *
 * PARAM's FIFO field turned out to be CORRECT (bits 11:8 = 3 -> 2^3 = 8 words,
 * which is exactly MCXN_SAI_FIFO_DEPTH).  The value was right; the LABEL was
 * the lie, and the fact that it was right was LUCK -- a hand-typed constant
 * sitting next to the real depth, free to drift the moment either changed.
 *
 *   ⭐ A CAPABILITY REGISTER THAT IS A CONSTANT CAN DRIFT FROM THE THING IT
 *     DESCRIBES.  ONE COMPUTED FROM IT CANNOT.  (The old comment even said
 *     "FIFO=32" while encoding 8.  The comment had ALREADY drifted from the
 *     value it was describing.)
 *
 * VERID is a different problem and it is NOT fixed here: the RM does not give
 * it, and a version register is exactly where 91emulator found themselves
 * "reaching into the driver to switch off a branch I did not want to
 * implement".  Ours is not doing that -- nothing in the SAI path branches on
 * it -- but it remains UNVERIFIED and is named as such rather than dressed up.
 */
/* 2^3 = 8 = MCXN_SAI_FIFO_DEPTH; asserted below */
#define SAI_FIFO_EXP     3u
/* ⚠ UNVERIFIED: the RM does not document VERID */
#define SAI_VERID_VALUE  0x03010000u
/* frame=5, FIFO=2^3, 2 chans */
#define SAI_PARAM_VALUE  ((5u << 16) | (SAI_FIFO_EXP << 8) | 2u)

/*
 * Assert against the ARRAY, not against another name for its size -- see the
 * long note in hw/char/mcxn_lpuart.c.  `>` because holding more than you
 * advertise is SAFE.
 */
QEMU_BUILD_BUG_ON((1u << SAI_FIFO_EXP) >
                  ARRAY_SIZE(((MCXNSAIState *)0)->tx_fifo));

/*
 * TCR2[BCD] (bit 24): 0 = bit clock generated EXTERNALLY (Target mode)
 *                     1 = generated INTERNALLY from MCLK (Controller mode)
 * TCR2[BYP] (bit 23): 1 = BYPASS the divider; the bit clock is
 *                     divide-by-one of MCLK.
 */
#define TCR2_BCD     (1u << 24)
#define TCR2_BYP     (1u << 23)

/*
 * The word period, derived from the registers firmware programmed:
 *   bit clock = MCLK / (2 * (TCR2[DIV] + 1)),  a word is TCR5[W0W] + 1 bits.
 *
 * ⚠ THE DIVIDER USED TO BE APPLIED UNCONDITIONALLY, AND THAT IS TWO BUGS.
 *
 * ① TCR2[BCD] = 0 IS *TARGET* MODE: THE BIT CLOCK COMES FROM OUTSIDE
 *    THE CHIP.
 *    The RM: "0b - Generate externally in Target mode."  On a codec board the
 *    wm8962 drives BCLK/LRCLK and the rate is set in the CODEC over I2C -- it
 *    is NOT DERIVABLE FROM ANY SAI REGISTER.  We were dividing MCLK anyway and
 *    producing a word rate the hardware would never have generated.
 *
 *      ⭐ CHECK BCD BEFORE YOU WRITE ONE LINE OF DIVIDER MATH.  IF IT IS 0,
 *        THE ANSWER IS NOT IN THIS DEVICE.               (93emulator)
 *
 *    93 nearly shipped that exact divider formula as a fix -- correct,
 *    RM-cited, and a FABRICATION on their board, because it agrees with itself
 *    at the one rate anyone tests.  ⭐ A FORMULA THAT IS CORRECT AT THE POINT
 *    YOU TESTED IT IS NOT A FORMULA YOU HAVE TESTED.
 *
 *    ⚠ AND OUR OWN TEST HAD THE SAME MISUNDERSTANDING: it wrote TCR2 WITHOUT
 *      BCD -- i.e. configured the SAI as a TARGET -- and then asserted the
 *      CONTROLLER's divider math.  Both halves of the loop shared the same
 *      wrong belief, which is exactly why it was green.  (95emulator's "my
 *      self-test booted three copies of my own tool.")
 *
 *    ⇒ STATED GAP: there is no codec model on this board, so in Target mode
 *      there is NO BIT CLOCK AT ALL.  We return 0 -- no transfer -- and say so
 *      on the guest-visible error channel, rather than inventing a clock the
 *      board does not have.  NEVER INVENT A PEER.
 *
 * ② TCR2[BYP] = 1 BYPASSES THE DIVIDER ("the internal bit clock is
 *    divide-by-one").
 *    We divided anyway.
 *
 * And the two fallbacks that used to live here -- `if (!bclk) bclk = 1;` and
 * `ns < 100 ? 100 : ns` -- are GONE.  A `?:` is not a safety net; it is a
 * place for a bug to live where no test will ever look.  A clock that is not
 * running must be VISIBLY not running, not floored to a plausible tick.
 */
static int64_t sai_word_period_ns(MCXNSAIState *s)
{
    uint32_t tcr2 = s->regs[SAI_TCR2 >> 2];
    uint32_t bits = TCR5_W0W(s->regs[SAI_TCR5 >> 2]) + 1;
    uint64_t bclk;

    if (!(tcr2 & TCR2_BCD)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mcxn_sai: TCR2[BCD]=0 (Target mode): the bit "
                      "clock is generated EXTERNALLY and this board has "
                      "no codec model -- there is no bit clock, so no "
                      "data is clocked. Set BCD=1 for Controller "
                      "mode.\n");
        return 0;   /* no clock: the transmitter does not run */
    }

    {
        uint32_t mclk = s->clk ? clock_get_hz(s->clk) : 0;

        if (tcr2 & TCR2_BYP) {
            bclk = mclk;                   /* divider bypassed: divide-by-one */
        } else {
            bclk = mclk / (2u * (TCR2_DIV(tcr2) + 1u));
        }
    }

    if (!bclk) {
        return 0;                      /* no clock is no clock -- say so */
    }
    return (int64_t)((uint64_t)bits * 1000000000ULL / bclk);
}

/* TCSR/RCSR flags computed from REAL FIFO occupancy, not asserted. */
static uint32_t sai_tcsr_flags(MCXNSAIState *s)
{
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    uint32_t wm = TCR1_TFW(s->regs[SAI_TCR1 >> 2]);
    uint32_t f = tcsr & CSR_STICKY_FLAGS;      /* FEF/SEF/WSF are sticky, W1C */

    /* The transmit FIFO asks for data when it has drained to the watermark. */
    if (s->tx_count <= wm) {
        f |= CSR_FRF | CSR_FWF;
    }
    return f;
}

static uint32_t sai_rcsr_flags(MCXNSAIState *s)
{
    uint32_t rcsr = s->regs[SAI_RCSR >> 2];
    uint32_t wm = RCR1_RFW(s->regs[SAI_RCR1 >> 2]);
    uint32_t f = rcsr & CSR_STICKY_FLAGS;

    /* The receive FIFO asks to be drained once it has filled past the mark. */
    if (s->rx_count > wm) {
        f |= CSR_FRF | CSR_FWF;
    }
    return f;
}

static void mcxn_sai_update_irq(MCXNSAIState *s)
{
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    uint32_t rcsr = s->regs[SAI_RCSR >> 2];
    uint32_t tflags = sai_tcsr_flags(s);
    uint32_t rflags = sai_rcsr_flags(s);
    bool tx = (((tflags >> 16) & 0x1Fu) &
               ((tcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;
    bool rx = (((rflags >> 16) & 0x1Fu) &
               ((rcsr >> CSR_IE_TO_FLAG_SHIFT) & 0x1Fu)) != 0;

    /*
     * THE DMA REQUEST LINES.  The same FIFO-request condition that raises the
     * interrupt also asks the eDMA for service when TCSR/RCSR[FRDE] is set —
     * this is how every stock SAI driver actually moves audio
     * (SAI_TransferSendEDMA, Zephyr's i2s_mcux_sai).  Without these lines the
     * eDMA could only be started by a software TCD_CSR[START] write, so
     * DMA-driven audio did not work at all: the guest armed a channel, enabled
     * ERQ, and waited forever for a request that nothing could raise.
     *
     * Level-driven and edge-suppressed: only a CHANGE is published, because a
     * qemu_irq handler runs on every qemu_set_irq call and the eDMA re-enters
     * us as it fills the FIFO.
     */
    bool tdma = (tflags & CSR_FRF) && (tcsr & CSR_FRDE);
    bool rdma = (rflags & CSR_FRF) && (rcsr & CSR_FRDE);

    if (tdma != s->tx_dma_req) {
        s->tx_dma_req = tdma;
        qemu_set_irq(s->dma_req_tx, tdma);
    }
    if (rdma != s->rx_dma_req) {
        s->rx_dma_req = rdma;
        qemu_set_irq(s->dma_req_rx, rdma);
    }

    qemu_set_irq(s->irq, tx || rx);
}

/*
 * One word leaves the transmit FIFO.  If the transmit FIFO is empty while the
 * transmitter is enabled, that is an UNDERRUN — real firmware must keep up,
 * and a model that silently invents a word to send would hide a real-time
 * budget that does not close on hardware.
 */
static void mcxn_sai_word_tick(void *opaque)
{
    int64_t period;
    MCXNSAIState *s = opaque;
    uint32_t tcsr = s->regs[SAI_TCSR >> 2];
    uint32_t rcsr = s->regs[SAI_RCSR >> 2];
    uint32_t word;

    if (!(tcsr & CSR_EN)) {
        return;                              /* transmitter disabled */
    }

    if (s->tx_count == 0) {
        s->regs[SAI_TCSR >> 2] |= CSR_FEF;   /* underrun: nothing to send */
    } else {
        word = s->tx_fifo[0];
        memmove(&s->tx_fifo[0], &s->tx_fifo[1],
                (--s->tx_count) * sizeof(s->tx_fifo[0]));

        /*
         * The word goes out of SAI_TXD.  With the bench jumper fitted it comes
         * straight back in on SAI_RXD; without it, it leaves the chip and there
         * is nothing on the other end, exactly as on a board with no codec.
         */
        if (s->loopback && (rcsr & CSR_EN)) {
            if (s->rx_count < MCXN_SAI_FIFO_DEPTH) {
                s->rx_fifo[s->rx_count++] = word;
            } else {
                s->regs[SAI_RCSR >> 2] |= CSR_FEF;   /* receive overrun */
            }
        }
    }

    /*
     * Re-arm from the DEADLINE: a word clock that re-adds its dispatch latency
     * every word drifts, and the sample rate is a contract.
     *
     * ⚠ AND A PERIOD OF ZERO MUST NOT BE ARMED.  sai_word_period_ns() returns
     *   0 when there is NO BIT CLOCK (Target mode, no codec).
     *   `next_word_ns += 0` does not advance, so timer_mod() would schedule a
     *   deadline already in the past, fire immediately, and do it again --
     *   forever.
     *
     *     ⭐ RETURNING 0 FOR "NO CLOCK" TURNS A DEAD CLOCK INTO AN INFINITE
     *       ONE.
     *
     *   I introduced exactly that while removing a `?:` floor that had been
     *   hiding the degenerate case.  A clock that is not running must not be
     *   running -- not running INFINITELY FAST.
     */
    period = sai_word_period_ns(s);
    if (period > 0) {
        s->next_word_ns += period;
        timer_mod(&s->word_timer, s->next_word_ns);
    } else {
        /* no clock: the transmitter does not run */
        timer_del(&s->word_timer);
    }
    mcxn_sai_update_irq(s);
}

static void sai_fifo_reset(MCXNSAIState *s, bool tx, bool rx)
{
    if (tx) {
        s->tx_count = 0;
        s->regs[SAI_TCSR >> 2] &= ~CSR_FEF;
    }
    if (rx) {
        s->rx_count = 0;
        s->regs[SAI_RCSR >> 2] &= ~CSR_FEF;
    }
}

static uint64_t mcxn_sai_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNSAIState *s = MCXN_SAI(opaque);
    uint32_t v = (off < MCXN_SAI_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case SAI_VERID:
        return SAI_VERID_VALUE;
    case SAI_PARAM:
        return SAI_PARAM_VALUE;
    case SAI_TCSR:
        /* Soft-reset bits are momentary; the FIFO flags are REAL. */
        v &= ~(CSR_SR | CSR_FR);
        v &= ~(CSR_FRF | CSR_FWF | CSR_FEF | CSR_SEF | CSR_WSF);
        v |= sai_tcsr_flags(s);
        return v;
    case SAI_RCSR:
        v &= ~(CSR_SR | CSR_FR);
        v &= ~(CSR_FRF | CSR_FWF | CSR_FEF | CSR_SEF | CSR_WSF);
        v |= sai_rcsr_flags(s);
        return v;
    case SAI_TFR0:
    case SAI_TFR0 + 4:
        /* Real read/write pointers: the gap between them IS the occupancy. */
        return (s->tx_count & 0xF) << 16;
    case SAI_RFR0:
    case SAI_RFR0 + 4:
        return (s->rx_count & 0xF) << 16;
    case SAI_RDR0:
    case SAI_RDR0 + 4: {
        uint32_t word;

        if (s->rx_count == 0) {
            /*
             * Reading an empty receive FIFO is an UNDERRUN.  Flagging it beats
             * handing back a plausible zero the firmware cannot tell from a
             * genuine sample of silence.
             */
            s->regs[SAI_RCSR >> 2] |= CSR_FEF;
            mcxn_sai_update_irq(s);
            return 0;
        }
        word = s->rx_fifo[0];
        memmove(&s->rx_fifo[0], &s->rx_fifo[1],
                (--s->rx_count) * sizeof(s->rx_fifo[0]));
        mcxn_sai_update_irq(s);
        return word;
    }
    default:
        return v;
    }
}

static void mcxn_sai_write(void *opaque, hwaddr off, uint64_t value,
                           unsigned size)
{
    MCXNSAIState *s = MCXN_SAI(opaque);
    uint32_t val = value;

    if (off >= MCXN_SAI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case SAI_VERID:
    case SAI_PARAM:
        return;   /* read-only */
    case SAI_TFR0:
    case SAI_TFR0 + 4:
    case SAI_RFR0:
    case SAI_RFR0 + 4:
    case SAI_RDR0:
    case SAI_RDR0 + 4:
        return;   /* read-only FIFO/data registers */
    case SAI_TCSR:
    case SAI_RCSR: {
        uint32_t cur = s->regs[off >> 2];
        bool was_te = (s->regs[SAI_TCSR >> 2] & CSR_EN) != 0;
        bool is_tx = (off == SAI_TCSR);

        /* Status flag bits are write-1-to-clear; clear those the guest set. */
        cur &= ~(val & CSR_FLAGS_W1C);
        /* Control bits (including TE/RE) latch from the write. */
        cur = (cur & CSR_FLAGS_W1C) | (val & ~CSR_FLAGS_W1C);
        /*
         * Soft-reset bits self-clear immediately, but they DO reset the FIFO.
         */
        if (val & (CSR_SR | CSR_FR)) {
            sai_fifo_reset(s, is_tx, !is_tx);
        }
        cur &= ~(CSR_SR | CSR_FR);
        s->regs[off >> 2] = cur;

        if (is_tx) {
            bool te = (cur & CSR_EN) != 0;

            if (te && !was_te) {
                /*
                 * The bit clock starts: words now leave the FIFO at the rate
                 * firmware configured.  Anchor the first deadline.
                 *
                 * ⚠ UNLESS THERE IS NO BIT CLOCK.  In Target mode
                 *   (TCR2[BCD]=0) the clock comes from an external codec this
                 *   board does not have, so ENABLING the transmitter starts
                 *   NOTHING -- exactly as on silicon, where TE with no BCLK
                 *   clocks no data.  Arming a zero-period timer here would
                 *   fire it immediately and forever.
                 */
                int64_t period = sai_word_period_ns(s);

                if (period > 0) {
                    s->next_word_ns =
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period;
                    timer_mod(&s->word_timer, s->next_word_ns);
                } else {
                    timer_del(&s->word_timer);
                }
            } else if (!te && was_te) {
                timer_del(&s->word_timer);
            }
        }
        mcxn_sai_update_irq(s);
        return;
    }
    case SAI_TDR0:
    case SAI_TDR0 + 4:
        if (s->tx_count >= MCXN_SAI_FIFO_DEPTH) {
            /*
             * Writing a full FIFO OVERRUNS: the word is lost.  Accepting it
             * silently is how a model lets firmware push more audio than the
             * hardware could ever have carried.
             */
            s->regs[SAI_TCSR >> 2] |= CSR_FEF;
        } else {
            s->tx_fifo[s->tx_count++] = val;
        }
        mcxn_sai_update_irq(s);
        return;
    default:
        s->regs[off >> 2] = val;
        return;
    }
}

static const MemoryRegionOps mcxn_sai_ops = {
    .read = mcxn_sai_read,
    .write = mcxn_sai_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_sai_reset(DeviceState *dev)
{
    MCXNSAIState *s = MCXN_SAI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->tx_count = s->rx_count = 0;
    timer_del(&s->word_timer);
    qemu_set_irq(s->irq, 0);
}

static void mcxn_sai_init(Object *obj)
{
    MCXNSAIState *s = MCXN_SAI(obj);

    /*
     * SAI function clock (MCLK) input — the SoC connects it to SYSCON's
     * SAIn-clk.
     */
    s->clk = qdev_init_clock_in(DEVICE(obj), "clk", NULL, NULL, 0);
}

static void mcxn_sai_realize(DeviceState *dev, Error **errp)
{
    MCXNSAIState *s = MCXN_SAI(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_sai_ops, s,
                          TYPE_MCXN_SAI, MCXN_SAI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req_tx); /* -> eDMA src 100 */
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->dma_req_rx); /* -> eDMA src 99  */

    timer_init_ns(&s->word_timer, QEMU_CLOCK_VIRTUAL, mcxn_sai_word_tick, s);
}

static const Property mcxn_sai_props[] = {
    /*
     * Wire SAI_TXD back to SAI_RXD — the digital equivalent of jumpering the
     * two pins on the bench, which is how a SAI is exercised on a board with
     * no codec attached.  This is a BOARD-LEVEL option, not a register: the
     * MCX N SAI has no loopback bit (the RM gives one to LPUART and to
     * FlexCAN, and none to the SAI), and inventing one would be fabricating
     * silicon.  Default off.
     */
    DEFINE_PROP_BOOL("loopback", MCXNSAIState, loopback, false),
};

/* Re-drive the IRQ line from restored register state after migration: the
 * output line is not part of vmstate, so a VM migrated with an enabled TCSR/RCSR
 * flag asserted would otherwise land with the line low. */
static int mcxn_sai_post_load(void *opaque, int version_id)
{
    mcxn_sai_update_irq(MCXN_SAI(opaque));
    return 0;
}

static const VMStateDescription vmstate_mcxn_sai = {
    .name = TYPE_MCXN_SAI,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mcxn_sai_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSAIState, MCXN_SAI_SIZE / 4),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_sai_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_sai_realize;
    device_class_set_legacy_reset(dc, mcxn_sai_reset);
    device_class_set_props(dc, mcxn_sai_props);
    dc->vmsd = &vmstate_mcxn_sai;
}

static const TypeInfo mcxn_sai_types[] = {
    {
        .name          = TYPE_MCXN_SAI,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSAIState),
        .instance_init = mcxn_sai_init,
        .class_init    = mcxn_sai_class_init,
    },
};

DEFINE_TYPES(mcxn_sai_types)
