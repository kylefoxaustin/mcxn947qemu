/*
 * NXP MCX N eDMA (enhanced DMA) — functional model.  See header.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/dma/mcxn_edma.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "qemu/bswap.h"

/* Management-page registers. */
#define R_MP_CSR  0x00
#define R_MP_ES   0x04
#define R_MP_INT  0x08
#define R_MP_HRS  0x0C
#define R_CH_GRPRI 0x100   /* [16] */

/* Per-channel register offsets (within the channel's 0x1000 block). */
#define R_CH_CSR   0x00
#define R_CH_ES    0x04
#define R_CH_INT   0x08
#define R_CH_SBR   0x0C

/*
 * CH_SBR[MID] (bits 4:0, CMSIS DMA_CH_SBR_MID_MASK) is the BUS MASTER ID, and its reset
 * value is PER-INSTANCE: the RM gives DMA0 6 and DMA1 7.  We reset it to ZERO -- a
 * different bus master entirely.
 *
 * ⭐ AND CH_SBR IS A REGISTER THE GUEST READ-MODIFY-WRITES.  Linux's fsl-edma does:
 *
 *       val  = edma_readl_chreg(chan, ch_sbr);   // reads OUR value
 *       val |= EDMA_V3_CH_SBR_RD;                // ORs its own direction bit in
 *       edma_writel_chreg(chan, val, ch_sbr);    // writes it back
 *
 * so it READ OUR ZERO AND WROTE IT BACK AS ITS OWN CONFIGURATION -- every subsequent
 * transfer issued under the wrong master ID (wrong bus attributes, wrong security
 * domain).  Nothing failed here, because nothing in THIS model reads those bits.  It
 * fails on hardware.
 *
 *     ⭐ A REGISTER THE GUEST READ-MODIFY-WRITES IS THE ONE PLACE A ZERO RESET VALUE
 *        SURVIVES INTO THE GUEST'S OWN STATE.  A zero reset is not the absence of a
 *        claim -- and the RMW registers are where the claim gets LAUNDERED.
 *                                                    (91emulator, same bug, same week)
 *
 * ⚠ AND THE RESET-VALUE GATE COULD NOT SEE THIS.  Its golden stores ONE reset per
 * register name and SHARES IT ACROSS INSTANCES, so the RM giving CH_SBR two values (6
 * and 7) looked like a CONTRADICTION and the extractor REFUSED all 16 of them --
 * correctly, because a wrong golden makes the checker lie.  But A REFUSAL IS NOT A
 * CHECK: the whole refusal pile turned out to be exactly this register, and nobody was
 * looking at it.  GREP YOUR GOLDEN FOR WHAT THE EXTRACTOR SKIPPED.
 */
#define CH_SBR_MID_MASK  0x1Fu
#define EDMA_MID_DMA0    6u   /* RM: DMA0 CH_SBR reset = 0000_0006h */
#define EDMA_MID_DMA1    7u   /* RM: DMA1 CH_SBR reset = 0000_0007h */
#define R_CH_PRI   0x10
#define R_CH_MUX   0x14
#define R_TCD_SADDR 0x20
#define R_TCD_SOFF  0x24
#define R_TCD_ATTR  0x26
#define R_TCD_NBYTES 0x28
#define R_TCD_SLAST 0x2C
#define R_TCD_DADDR 0x30
#define R_TCD_DOFF  0x34
#define R_TCD_CITER 0x36
#define R_TCD_DLAST 0x38
#define R_TCD_CSR   0x3C
#define R_TCD_BITER 0x3E

/* CH_ES — error status (CMSIS DMA_CH_ES_*).  The eDMA VALIDATES THE TCD BEFORE
 * IT MOVES ANYTHING and refuses to run on an error; it does not quietly move a
 * partial minor loop. */
#define CH_ES_SGE    (1u << 2)    /* scatter/gather configuration error */
#define CH_ES_NCE    (1u << 3)    /* NBYTES/CITER configuration error */
#define CH_ES_DOE    (1u << 4)    /* destination offset error         */
#define CH_ES_DAE    (1u << 5)    /* destination address error        */
#define CH_ES_SOE    (1u << 6)    /* source offset error              */
#define CH_ES_SAE    (1u << 7)    /* source address error             */
#define CH_ES_ERR    (1u << 31)   /* any of the above                 */

#define CH_CSR_ERQ   (1u << 0)
#define CH_CSR_EEI   (1u << 2)   /* error interrupt enable (CMSIS DMA_CH_CSR_EEI) */
#define CH_CSR_DONE  (1u << 30)
#define CH_INT_INT   (1u << 0)
#define TCD_CSR_START    (1u << 0)
#define TCD_CSR_INTMAJOR (1u << 1)
#define TCD_CSR_DREQ     (1u << 3)   /* CMSIS DMA_TCD_CSR_DREQ_MASK */
#define TCD_CSR_ESG      (1u << 4)   /* enable scatter/gather: load the next TCD */
#define TCD_CSR_MAJORELINK (1u << 5) /* link a channel on major completion       */
#define TCD_CSR_MAJORLINKCH(v)  (((v) >> 8) & 0xF)

/* ATTR: SMOD[15:11] / SSIZE[10:8] / DMOD[7:3] / DSIZE[2:0]  (CMSIS DMA_TCD_ATTR_*)
 * SMOD/DMOD define a CIRCULAR BUFFER: the address wraps inside a 2^MOD-aligned
 * block.  These were NOT MODELLED, so the stock wrap_transfer example ran off the
 * end of its source buffer and handed the guest ADJACENT MEMORY -- 150000000,
 * 16000000, 32768: the clock constants that happened to live next door.  Not
 * zeros.  PLAUSIBLE NUMBERS.  A silent wrong answer with real data in it. */
#define ATTR_SMOD(a)   (((a) >> 11) & 0x1F)
#define ATTR_DMOD(a)   (((a) >> 3) & 0x1F)

/* CITER/BITER: when ELINK is set the count is only 9 bits -- the upper bits are
 * the link channel.  Masking with 0x7FFF would read LINKCH AS PART OF THE COUNT. */
#define CITER_ELINK      (1u << 15)
#define CITER_ELINK_MASK 0x1FFu
#define CITER_LINKCH(v)  (((v) >> 9) & 0xF)
#define ATTR_SSIZE(a)  (((a) >> 8) & 0x7)
#define ATTR_DSIZE(a)  ((a) & 0x7)
#define NBYTES_MASK  0x3FFFFFFFu
#define CITER_MASK   0x7FFFu

/*
 * Advance an address by its offset, honouring the ATTR modulo (circular buffer).
 * A zero MOD means a plain linear advance.  With MOD = k the address wraps inside
 * its own 2^k-aligned block, which is exactly what makes a ring buffer a ring.
 */
static uint32_t edma_advance(uint32_t addr, int16_t off, unsigned mod)
{
    uint32_t mask;

    if (mod == 0) {
        return addr + off;
    }
    mask = (1u << mod) - 1;
    return (addr & ~mask) | ((addr + off) & mask);
}

/* Forward decl: a linked channel is started by setting its TCD_CSR[START]. */
static void edma_run(MCXNEDMAState *s, int n);

/*
 * CHANNEL LINKING.  On minor- or major-loop completion a channel can start
 * ANOTHER channel.  This was not modelled at all, so the stock `channel_link`
 * example moved NOTHING -- both destination buffers came back all zeros.
 */
static void edma_link_channel(MCXNEDMAState *s, unsigned link)
{
    if (link >= MCXN_EDMA_CHANNELS || s->in_link) {
        return;                           /* bound the chain: no link loops */
    }
    s->in_link = true;
    s->ch[link].csr &= ~CH_CSR_DONE;
    edma_run(s, link);
    s->in_link = false;
}

/*
 * SCATTER/GATHER.  With TCD_CSR[ESG], TCD_DLAST_SGA is not a signed adjustment --
 * IT IS A POINTER TO THE NEXT TCD, which the engine loads on major completion and
 * then runs.  Not modelled, so the stock `scatter_gather` and `ping_pong`
 * examples ran only their FIRST TCD: destination came back "1 2 3 4 0 0 0 0",
 * half the transfer silently missing.
 *
 * The in-memory TCD is 32 bytes, laid out exactly as the channel's TCD registers.
 */
static bool edma_load_next_tcd(MCXNEDMAState *s, int n)
{
    MCXNEDMAChan *c = &s->ch[n];
    hwaddr sga = c->tcd_dlast;            /* DLAST_SGA doubles as the pointer */
    uint8_t tcd[32];

    if (sga == 0 || (sga & 0x1F)) {
        c->es |= CH_ES_SGE | CH_ES_ERR;   /* scatter/gather config error */
        return false;
    }
    if (dma_memory_read(&address_space_memory, sga, tcd, sizeof(tcd),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        c->es |= CH_ES_SGE | CH_ES_ERR;
        return false;
    }

    c->tcd_saddr  = ldl_le_p(tcd + 0x00);
    c->tcd_soff   = lduw_le_p(tcd + 0x04);
    c->tcd_attr   = lduw_le_p(tcd + 0x06);
    c->tcd_nbytes = ldl_le_p(tcd + 0x08);
    c->tcd_slast  = ldl_le_p(tcd + 0x0C);
    c->tcd_daddr  = ldl_le_p(tcd + 0x10);
    c->tcd_doff   = lduw_le_p(tcd + 0x14);
    c->tcd_citer  = lduw_le_p(tcd + 0x16);
    c->tcd_dlast  = ldl_le_p(tcd + 0x18);
    c->tcd_csr    = lduw_le_p(tcd + 0x1C);
    c->tcd_biter  = lduw_le_p(tcd + 0x1E);
    return true;
}

static void edma_update_irq(MCXNEDMAState *s, int n)
{
    qemu_set_irq(s->irq[n], !!(s->ch[n].intr & CH_INT_INT));
}

/*
 * Run ONE MINOR LOOP (NBYTES) of a channel and advance the TCD.  Returns true
 * once the MAJOR loop completes.
 *
 * This is the unit a HARDWARE REQUEST moves.  Real eDMA does not transfer a
 * whole buffer when a peripheral asks for service: a SAI whose FIFO dropped
 * below its watermark asks for ONE minor loop, the DMA moves NBYTES into the
 * FIFO, and the peripheral asks again when it next needs data.  That is the
 * whole shape of DMA-driven audio/ADC/UART, and modelling only the
 * software-START path (which ran the entire major loop in one go) meant this
 * device could not do it at all.
 */
static bool edma_minor_loop(MCXNEDMAState *s, int n)
{
    MCXNEDMAChan *c = &s->ch[n];
    uint32_t ssize = 1u << ATTR_SSIZE(c->tcd_attr);
    uint32_t dsize = 1u << ATTR_DSIZE(c->tcd_attr);
    uint32_t nbytes = c->tcd_nbytes & NBYTES_MASK;
    unsigned smod = ATTR_SMOD(c->tcd_attr);
    unsigned dmod = ATTR_DMOD(c->tcd_attr);
    bool elink = (c->tcd_citer & CITER_ELINK) != 0;
    uint32_t cmask = elink ? CITER_ELINK_MASK : CITER_MASK;
    uint32_t citer = c->tcd_citer & cmask;
    int16_t soff = (int16_t)c->tcd_soff;
    int16_t doff = (int16_t)c->tcd_doff;
    uint32_t saddr = c->tcd_saddr, daddr = c->tcd_daddr;
    uint8_t buf[MCXN_EDMA_MAX_XFER];
    uint32_t chunk;
    uint32_t err;
    uint32_t b;

    if (dsize == 0 || nbytes == 0 || citer == 0) {
        /* CITER=0 / NBYTES=0 is a CONFIGURATION ERROR, not a completed transfer.
         * Setting DONE here told the guest an empty TCD had "finished". */
        c->es |= CH_ES_NCE | CH_ES_ERR;
        if (c->csr & CH_CSR_EEI) {
            c->intr |= CH_INT_INT;
            edma_update_irq(s, n);
        }
        return true;
    }

    /*
     * ⚠ CONFIGURATION CHECK — and this used to be a SILENT DATA LOSS.
     *
     * The minor loop below is `for (b = 0; b + step <= nbytes; b += step)`.  With
     * NBYTES=6 and a 4-byte transfer size it moved FOUR BYTES AND DROPPED TWO --
     * no error, no flag, DONE set, INTMAJOR raised, and the guest told that its
     * transfer had completed.  Real eDMA does not do that: a NBYTES that is not a
     * multiple of the transfer size is a CONFIGURATION ERROR (CH_ES[NCE]) and the
     * channel REFUSES TO RUN.
     *
     * ⭐ EVERY TEST IN THIS TREE USED NBYTES = 4 WITH A 4-BYTE TRANSFER SIZE.
     * Perfectly round, and the bug is invisible at every round value.  Found by
     * carrying ollama_95_neutron's rule one column to the right:
     *   "ROUND NUMBERS ARE HOW BUGS SURVIVE -- a 2^n sweep sails straight past the
     *    broken values.  I learned the lesson ON the axis that taught it, and did
     *    not carry it ONE COLUMN TO THE RIGHT."
     */
    /*
     * ⚠ THE FULL TCD VALIDATION -- and I nearly shipped a DEAD ERROR CHANNEL while
     * fixing one.  When the NBYTES check went in I DEFINED CH_ES[SAE], [SOE],
     * [DAE] and [DOE] and IMPLEMENTED NONE OF THEM: four error bits that could
     * never be set, in a register a driver reads to find out what went wrong.
     * Exactly the "defined but never raised" class I had swept for hours earlier.
     *
     * ollama_95_neutron, on his N axis: "I am not predicting it is broken.  I am
     * saying I HAVE NO RIGHT TO SAY IT ISN'T."  My SOFF/SADDR axes had had the
     * same treatment -- every test uses SOFF=4 and an aligned buffer.  Round.
     *
     * Real eDMA validates the whole TCD before it moves anything (RM): a
     * misaligned address, an offset that is not a multiple of the transfer size,
     * a NBYTES that is not a multiple of it, or CITER=0, and the channel REFUSES
     * TO RUN.  Silently walking a misaligned buffer 3 bytes at a time produces
     * garbage that no firmware could diagnose.
     */
    err = 0;
    if (saddr % ssize) {
        err |= CH_ES_SAE;                 /* source address not aligned      */
    }
    if (soff % (int16_t)ssize) {
        err |= CH_ES_SOE;                 /* source offset not a multiple    */
    }
    if (daddr % dsize) {
        err |= CH_ES_DAE;                 /* dest address not aligned        */
    }
    if (doff % (int16_t)dsize) {
        err |= CH_ES_DOE;                 /* dest offset not a multiple      */
    }
    if (nbytes % ssize || nbytes % dsize) {
        err |= CH_ES_NCE;                 /* NBYTES not a multiple           */
    }
    if (ssize > MCXN_EDMA_MAX_XFER || dsize > MCXN_EDMA_MAX_XFER) {
        err |= CH_ES_NCE;                 /* burst wider than the engine     */
    }

    if (err) {
        c->es |= err | CH_ES_ERR;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "mcxn-edma: ch%d TCD is invalid (CH_ES=0x%x): "
                      "SADDR=0x%x SOFF=%d DADDR=0x%x DOFF=%d NBYTES=%u "
                      "SSIZE=%u DSIZE=%u -- the channel does NOT run\n",
                      n, err, saddr, soff, daddr, doff, nbytes, ssize, dsize);
        /* The ERROR interrupt is gated by CH_CSR[EEI] -- NOT by TCD_CSR[INTMAJOR],
         * which is the TRANSFER-COMPLETE signal.  Raising major-completion on an
         * error would tell the guest "done" about a transfer that never ran,
         * which is the very lie this check exists to remove. */
        if (c->csr & CH_CSR_EEI) {
            c->intr |= CH_INT_INT;
            edma_update_irq(s, n);
        }
        return true;                      /* aborted: no DONE, no data moved */
    }

    /*
     * ⚠ SSIZE AND DSIZE ARE SEPARATE FIELDS, AND THIS USED TO IGNORE DSIZE
     * ENTIRELY.  The loop was `read(saddr, buf, step); write(daddr, buf, step);`
     * with step = SSIZE -- so the WRITE used the SOURCE width.  With SSIZE = 1 and
     * DSIZE = 4 the engine issued FOUR SEPARATE BYTE WRITES where the TCD asked
     * for ONE 32-BIT WRITE.
     *
     * To a MEMORY destination that is the same bytes and the bug is INVISIBLE.
     * To an MMIO PERIPHERAL REGISTER it is a completely different transaction:
     * four byte-pokes at a data register is not one word write -- a FIFO gets four
     * pushes instead of one, and a register with min_access_size = 4 rejects them
     * outright.  That is exactly the DMA-to-peripheral path this model now serves.
     *
     * ⭐ AND EVERY TEST IN THIS TREE SET SSIZE == DSIZE.  Each field was swept
     * ALONE and each was individually "correct"; the PAIR was never varied, so the
     * per-axis tests did not merely MISS this -- THEY CERTIFIED IT.
     * ollama_95_neutron, on exactly this shape: "An independent per-axis whitelist
     * will pass a shape that is garbage, with full confidence, because each axis is
     * individually safe.  That is the worst failure a gate can have.  It does not
     * merely MISS the bug -- IT CERTIFIES IT."
     *
     * Correct semantics: the minor loop moves NBYTES.  READS happen in SSIZE
     * chunks (SADDR advances by SOFF each), WRITES in DSIZE chunks (DADDR advances
     * by DOFF each) -- two independent strides.  Both sizes are powers of two, so
     * lcm(ssize, dsize) = max(ssize, dsize), and NBYTES is validated above to be a
     * multiple of both.
     */
    chunk = ssize > dsize ? ssize : dsize;

    for (b = 0; b < nbytes; b += chunk) {
        uint32_t k;

        for (k = 0; k < chunk; k += ssize) {
            address_space_read(&address_space_memory, saddr,
                               MEMTXATTRS_UNSPECIFIED, buf + k, ssize);
            saddr = edma_advance(saddr, soff, smod);
        }
        for (k = 0; k < chunk; k += dsize) {
            address_space_write(&address_space_memory, daddr,
                                MEMTXATTRS_UNSPECIFIED, buf + k, dsize);
            daddr = edma_advance(daddr, doff, dmod);
        }
    }
    c->tcd_saddr = saddr;
    c->tcd_daddr = daddr;

    citer--;
    c->tcd_citer = (c->tcd_citer & ~cmask) | citer;

    /*
     * MINOR-loop channel link -- but NOT on the LAST minor loop.
     *
     * ⚠ Per the RM the minor link fires at the end of each minor loop EXCEPT the
     * final one; when CITER reaches 0 the MAJOR loop completes and MAJORELINK
     * fires INSTEAD.  Firing both meant the linked channel ran ONE TIME TOO MANY,
     * and on its extra pass its destination address had already been advanced --
     * so it wrote PAST THE END of its buffer and corrupted the next object in
     * memory.  In the stock channel_link example that object was the driver's own
     * `g_Transfer_Done` flag: the ISR set it true, the spurious extra transfer
     * stamped it back to false, and main waited forever on a flag that HAD been
     * set.  The interrupt fired, the ISR ran, the CPU took exception 19 and
     * returned cleanly -- and the machine still hung, because the DMA was
     * overwriting the very variable the guest was polling.
     */
    if (citer != 0) {
        if (elink) {
            edma_link_channel(s, CITER_LINKCH(c->tcd_citer));
        }
        return false;                     /* major loop still running */
    }

    /* Major loop complete. */
    c->tcd_saddr = saddr + (int32_t)c->tcd_slast;
    if (!(c->tcd_csr & TCD_CSR_ESG)) {
        /* Without scatter/gather DLAST is a signed adjustment.  WITH it, DLAST_SGA
         * is a POINTER to the next TCD and must NOT be added to DADDR. */
        c->tcd_daddr = daddr + (int32_t)c->tcd_dlast;
    } else {
        c->tcd_daddr = daddr;
    }
    c->tcd_citer = c->tcd_biter;          /* reload major count */

    /* MAJOR-loop channel link. */
    if (c->tcd_csr & TCD_CSR_MAJORELINK) {
        edma_link_channel(s, TCD_CSR_MAJORLINKCH(c->tcd_csr));
    }

    /*
     * SCATTER/GATHER: load the next TCD.
     *
     * ⚠ AND THE CHAIN MUST YIELD TO THE GUEST BETWEEN TCDs.  A ping-pong chain is
     * CIRCULAR BY DESIGN (TCD A -> B -> A forever, a continuous double-buffer);
     * the driver's INTMAJOR handler counts iterations and stops it.  Running the
     * chain synchronously never lets that ISR execute, so the machine SPINS -- my
     * first version hung the stock ping_pong example outright, having "fixed"
     * scatter_gather (whose last TCD has ESG=0 and therefore terminates).
     *
     * So: load the next TCD, raise the major interrupt, and RE-ARM through the
     * bottom half.  The guest gets to run its ISR between links, exactly as it
     * does on silicon.
     */
    if (c->tcd_csr & TCD_CSR_ESG) {
        /*
         * ⚠ LOAD THE NEXT TCD -- BUT DO NOT RUN IT.
         *
         * Real eDMA loads the next TCD on major completion and then STOPS; the
         * channel must be re-triggered (by a request or a fresh START) to run it.
         * My first version RE-ARMED the channel itself, which chained the whole
         * list synchronously.  That "fixed" nothing (scatter_gather already worked
         * once the modulo and the ELINK-aware CITER were right) and it HUNG the
         * stock ping_pong example outright: a ping-pong chain is CIRCULAR by
         * design, and the NXP driver walks it from its own INTMAJOR handler.  My
         * hardware chaining double-processed a list the driver was already
         * managing, and the machine spun.
         *
         * Found by mutation: DISABLING scatter-gather entirely left the test GREEN
         * and made ping_pong PASS.  A feature I added, that was not needed, that
         * broke a working case -- and only breaking it on purpose showed me.
         */
        (void)edma_load_next_tcd(s, n);   /* SGE flagged inside on a bad pointer */
    }

    c->csr |= CH_CSR_DONE;
    if (c->tcd_csr & TCD_CSR_DREQ) {
        c->csr &= ~CH_CSR_ERQ;            /* auto-disable the request */
    }
    if (c->tcd_csr & TCD_CSR_INTMAJOR) {
        c->intr |= CH_INT_INT;
        edma_update_irq(s, n);
    }
    return true;
}

/*
 * A SERVICE REQUEST: run ONE MINOR LOOP.
 *
 * ⚠ THIS USED TO RUN THE ENTIRE MAJOR LOOP, AND THAT IS A CORE SEMANTICS BUG.
 * TCD_CSR[START] is a software-initiated SERVICE REQUEST -- exactly like a
 * peripheral DMA request -- and one request moves ONE MINOR LOOP (NBYTES), not
 * the whole major loop.  The engine then clears START.
 *
 * The stock channel_link example proves it: it calls EDMA_TriggerChannelStart
 * TWICE, because its channel has CITER = 2.  Two minor loops, two service
 * requests.  My model completed the WHOLE transfer on the first trigger, so the
 * second trigger RAN EVERYTHING AGAIN -- and by then the linked channels' SADDR
 * and DADDR had already advanced past their buffers:
 *
 *     1st trigger:  ch1 saddr=0x20000064 daddr=0x2000014c   correct
 *     2nd trigger:  ch1 saddr=0x20000074 daddr=0x2000015c   walked into destAddr2
 *                   ch2 saddr=0x20000074 daddr=0x2000016c   past the end entirely
 *
 * It wrote past the buffers, corrupted the guest's memory, and the CPU took a
 * HARD FAULT (CFSR = INVSTATE, HFSR = FORCED).  The "hang" I had been chasing was
 * never a hang: THE GUEST HAD CRASHED, and HardFault_Handler is a while(1).  An
 * exit-1 crash and an exit-1 refusal, one more time.
 *
 * A channel LINK is also a service request, so it moves one minor loop too.
 */
static void edma_run(MCXNEDMAState *s, int n)
{
    edma_minor_loop(s, n);
    s->ch[n].tcd_csr &= ~TCD_CSR_START;   /* the engine clears START */
}

static uint64_t edma_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNEDMAState *s = MCXN_EDMA(opaque);
    MCXNEDMAChan *c;
    int n;

    if (off < 0x1000) {
        switch (off) {
        case R_MP_CSR: return s->mp_csr;
        case R_MP_ES:  return s->mp_es;
        case R_MP_INT: {
            uint32_t r = 0;
            for (n = 0; n < MCXN_EDMA_CHANNELS; n++) {
                if (s->ch[n].intr & CH_INT_INT) {
                    r |= (1u << n);
                }
            }
            return r;
        }
        case R_MP_HRS: return 0;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * MCXN_EDMA_CHANNELS) {
                return s->ch_grpri[(off - R_CH_GRPRI) / 4];
            }
            return 0;
        }
    }

    n = (off / 0x1000) - 1;
    if (n >= MCXN_EDMA_CHANNELS) {
        return 0;
    }
    c = &s->ch[n];
    switch (off % 0x1000) {
    case R_CH_CSR:    return c->csr;
    case R_CH_ES:     return c->es;
    case R_CH_INT:    return c->intr;
    case R_CH_SBR:    return c->sbr;
    case R_CH_PRI:    return c->pri;
    case R_CH_MUX:    return c->mux;
    case R_TCD_SADDR: return c->tcd_saddr;
    case R_TCD_SOFF:  return c->tcd_soff;
    case R_TCD_ATTR:  return c->tcd_attr;
    case R_TCD_NBYTES: return c->tcd_nbytes;
    case R_TCD_SLAST: return c->tcd_slast;
    case R_TCD_DADDR: return c->tcd_daddr;
    case R_TCD_DOFF:  return c->tcd_doff;
    case R_TCD_CITER: return c->tcd_citer;
    case R_TCD_DLAST: return c->tcd_dlast;
    case R_TCD_CSR:   return c->tcd_csr;
    case R_TCD_BITER: return c->tcd_biter;
    default:          return 0;
    }
}

static void edma_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    MCXNEDMAState *s = MCXN_EDMA(opaque);
    MCXNEDMAChan *c;
    uint32_t v = val;
    int n;

    if (off < 0x1000) {
        switch (off) {
        case R_MP_CSR:
            s->mp_csr = v;
            return;
        default:
            if (off >= R_CH_GRPRI && off < R_CH_GRPRI + 4 * MCXN_EDMA_CHANNELS) {
                s->ch_grpri[(off - R_CH_GRPRI) / 4] = v;
            }
            return;
        }
    }

    n = (off / 0x1000) - 1;
    if (n >= MCXN_EDMA_CHANNELS) {
        return;
    }
    c = &s->ch[n];
    switch (off % 0x1000) {
    case R_CH_CSR:
        /* DONE is write-1-to-clear; keep the rest. */
        if (v & CH_CSR_DONE) {
            c->csr &= ~CH_CSR_DONE;
        }
        c->csr = (c->csr & CH_CSR_DONE) | (v & ~CH_CSR_DONE);
        return;
    case R_CH_INT:
        if (v & CH_INT_INT) {                 /* write-1-to-clear */
            c->intr &= ~CH_INT_INT;
            edma_update_irq(s, n);
        }
        return;
    case R_CH_ES:
        c->es &= ~v;                      /* W1C */
        return;
    case R_CH_SBR:
        c->sbr = v;
        return;
    case R_CH_PRI:
        c->pri = v;
        return;
    case R_CH_MUX:
        c->mux = v;
        return;
    case R_TCD_SADDR:
        c->tcd_saddr = v;
        return;
    case R_TCD_SOFF:
        c->tcd_soff = v;
        return;
    case R_TCD_ATTR:
        c->tcd_attr = v;
        return;
    case R_TCD_NBYTES:
        c->tcd_nbytes = v;
        return;
    case R_TCD_SLAST:
        c->tcd_slast = v;
        return;
    case R_TCD_DADDR:
        c->tcd_daddr = v;
        return;
    case R_TCD_DOFF:
        c->tcd_doff = v;
        return;
    case R_TCD_CITER:
        c->tcd_citer = v;
        return;
    case R_TCD_DLAST:
        c->tcd_dlast = v;
        return;
    case R_TCD_CSR:
        c->tcd_csr = v;
        if (v & TCD_CSR_START) {
            c->csr &= ~CH_CSR_DONE;
            edma_run(s, n);
        }
        return;
    case R_TCD_BITER:
        c->tcd_biter = v;
        return;
    default:
        return;
    }
}

/*
 * Service every channel whose hardware request line is asserted.
 *
 * ⚠ THIS MUST RUN IN A BOTTOM HALF, NOT INLINE FROM THE REQUEST.  A peripheral
 * raises its request from inside its OWN MMIO write handler (the SAI asserts
 * when firmware sets TCSR[FRDE]).  If the DMA then wrote straight back into that
 * peripheral, it would be a RE-ENTRANT MMIO access into a device already engaged
 * in I/O, and QEMU's re-entrancy guard DROPS IT:
 *
 *     qemu-system-arm: warning: Blocked re-entrant IO on MemoryRegion: mcxn-sai
 *                      at addr: 0x20
 *
 * The failure is SILENT DATA LOSS and it looks exactly like success: the channel
 * still walks its minor loops, still decrements CITER, still sets DONE and still
 * raises INTMAJOR -- while every byte it "moved" was thrown away and the FIFO
 * stayed empty.  A test that asked "did the transfer complete?" would pass.  Only
 * checking the DATA catches it.  Real DMA is asynchronous anyway; a bottom half
 * is both the correct model and the correct QEMU idiom.
 */
static void mcxn_edma_service_bh(void *opaque)
{
    MCXNEDMAState *s = opaque;
    int guard = 0;
    bool progress;

    do {
        int n;

        progress = false;

        for (n = 0; n < MCXN_EDMA_CHANNELS; n++) {
            MCXNEDMAChan *c = &s->ch[n];
            uint32_t src = c->mux & CH_MUX_SRC_MASK;

            if (!(c->csr & CH_CSR_ERQ) || src == 0) {
                continue;
            }
            if (!s->req_level[src]) {
                continue;               /* nothing is asking this channel */
            }
            if (!s->req_enabled[src]) {
                /* INPUTMUX_DMAn_REQ_ENABLE has this source's bit CLEAR: the
                 * request is blocked before it ever reaches the engine.  Silicon
                 * drops it here; so do we. */
                continue;
            }
            /*
             * One minor loop per request, as the hardware does.  Writing into
             * the peripheral makes it re-evaluate its FIFO and update its
             * request line (through mcxn_edma_req below), so the next pass of
             * this loop sees the new level.
             */
            c->csr &= ~CH_CSR_DONE;
            edma_minor_loop(s, n);
            progress = true;
            /*
             * An EDGE (timer-match) source fires once and moves exactly one minor
             * loop: auto-ack it here so the drain loop does not re-run it against a
             * level that no FIFO and no write-back will ever lower.  A FIFO source
             * (req_edge clear) keeps its level and is re-evaluated next pass. */
            if (s->req_edge[src]) {
                s->req_level[src] = false;
            }
        }
    } while (progress && ++guard < MCXN_EDMA_MAX_LOOPS);

    /*
     * A pulse (timer-match) is one-shot: the BH gets exactly one chance to service
     * it.  Any edge level still standing here was NOT consumed -- no channel selected
     * that source, or its INPUTMUX gate was closed -- so drop it.  Real match pulses
     * evaporate when nothing is listening; latching one would fire a spurious transfer
     * the moment a channel is later armed on that source. */
    {
        int src;

        for (src = 0; src < MCXN_EDMA_REQ_SOURCES; src++) {
            if (s->req_edge[src]) {
                s->req_level[src] = false;
            }
        }
    }
}

/*
 * A peripheral asserted (or dropped) its DMA request line.
 *
 * `src` is the MCX N request-mux source number from the CMSIS
 * dma_request_source_t enum (e.g. SAI0 Tx = 100, DAC0 = 25, ADC0 FIFO A = 21).
 * A channel consumes it when CH_MUX[SRC] selects that source and CH_CSR[ERQ]
 * enables the hardware request.  Until now ERQ was a DEAD CONSTANT -- defined,
 * never read -- and the only way to move a byte was a software TCD_CSR[START]
 * write.  That meant peripheral-triggered DMA, which is how essentially all real
 * audio/ADC/UART/SPI transfer works, DID NOT EXIST: a guest that set ERQ and
 * waited for the FIFO watermark to drive the transfer waited forever.
 *
 * We only latch the level here and kick the bottom half; see above for why the
 * transfer itself must not happen on this call stack.
 */
/*
 * INPUTMUX told us whether request source `src` is allowed through at all.
 * This is a GATE, not a request: it does not schedule anything by itself, but a
 * source that has just been re-enabled may already be asserting, so kick the
 * bottom half and let the service loop re-evaluate.
 */
static void mcxn_edma_req_enable(void *opaque, int src, int level)
{
    MCXNEDMAState *s = MCXN_EDMA(opaque);

    if (src < 0 || src >= MCXN_EDMA_REQ_SOURCES) {
        return;
    }
    if (s->req_enabled[src] == !!level) {
        return;
    }
    s->req_enabled[src] = level;
    if (level && s->req_level[src]) {
        qemu_bh_schedule(s->bh);
    }
}

static void mcxn_edma_req(void *opaque, int src, int level)
{
    MCXNEDMAState *s = MCXN_EDMA(opaque);

    if (src < 0 || src >= MCXN_EDMA_REQ_SOURCES) {
        return;
    }
    s->req_level[src] = level;
    if (level) {
        qemu_bh_schedule(s->bh);
    }
}

/*
 * An EDGE (pulse) request from a timer-match source (CTIMER, SCT).  The match is
 * an instantaneous event, not a FIFO level: it asks for exactly ONE minor loop and
 * has no level for the drain loop to lower afterwards.  We latch the level so the
 * bottom half sees it, mark the source as edge so the drain loop AUTO-ACKS it after
 * a single minor loop, and kick the BH.  Servicing is deferred to the BH for the
 * same reason as every other source: the request may arrive from the peripheral's
 * own MMIO write, and writing back on that stack is a re-entrant access QEMU drops.
 */
static void mcxn_edma_req_pulse(void *opaque, int src, int level)
{
    MCXNEDMAState *s = MCXN_EDMA(opaque);

    if (src < 0 || src >= MCXN_EDMA_REQ_SOURCES) {
        return;
    }
    if (!level) {
        return;                 /* a pulse: only the asserting edge carries meaning */
    }
    s->req_edge[src] = true;
    s->req_level[src] = true;
    qemu_bh_schedule(s->bh);
}

static const MemoryRegionOps edma_ops = {
    .read = edma_read,
    .write = edma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_edma_reset(DeviceState *dev)
{
    MCXNEDMAState *s = MCXN_EDMA(dev);

    /* MP_CSR reset = 0x0031_0000 (RM eDMA Management Page Control, both DMA0/DMA1):
     * read-only management-page capability bits [21,20,16] that are not named fields in
     * CMSIS and are not consumed by this model.  It was 0 -- a plausible-but-wrong reset the
     * reset-values gate covers now that the golden reaches the eDMA management page. */
    s->mp_csr = 0x00310000u;
    s->mp_es = 0;
    memset(s->ch_grpri, 0, sizeof(s->ch_grpri));
    memset(s->ch, 0, sizeof(s->ch));
    memset(s->req_level, 0, sizeof(s->req_level));
    memset(s->req_edge, 0, sizeof(s->req_edge));
    {
        int ch;

        for (ch = 0; ch < MCXN_EDMA_CHANNELS; ch++) {
            s->ch[ch].sbr = (s->dma_id ? EDMA_MID_DMA1 : EDMA_MID_DMA0)
                            & CH_SBR_MID_MASK;
        }
    }
    /* INPUTMUX resets with every request line ENABLED (see the header). */
    memset(s->req_enabled, 1, sizeof(s->req_enabled));
}

static void mcxn_edma_realize(DeviceState *dev, Error **errp)
{
    MCXNEDMAState *s = MCXN_EDMA(dev);
    int n;

    /* One input per MCX N DMA request-mux source: peripherals drive these. */
    qdev_init_gpio_in(dev, mcxn_edma_req, MCXN_EDMA_REQ_SOURCES);
    qdev_init_gpio_in_named(dev, mcxn_edma_req_enable, "req-enable",
                            MCXN_EDMA_REQ_SOURCES);
    qdev_init_gpio_in_named(dev, mcxn_edma_req_pulse, "req-pulse",
                            MCXN_EDMA_REQ_SOURCES);
    s->bh = qemu_bh_new(mcxn_edma_service_bh, s);

    /* Management page (0x0) + 16 channels x 0x1000 = 0x11000. */
    memory_region_init_io(&s->iomem, OBJECT(s), &edma_ops, s, TYPE_MCXN_EDMA,
                          0x1000 * (MCXN_EDMA_CHANNELS + 1));
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    for (n = 0; n < MCXN_EDMA_CHANNELS; n++) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq[n]);
    }
}

static const VMStateDescription vmstate_edma_chan = {
    .name = "mcxn-edma-chan",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(csr, MCXNEDMAChan),
        VMSTATE_UINT32(es, MCXNEDMAChan),
        VMSTATE_UINT32(intr, MCXNEDMAChan),
        VMSTATE_UINT32(sbr, MCXNEDMAChan),
        VMSTATE_UINT32(pri, MCXNEDMAChan),
        VMSTATE_UINT32(mux, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_saddr, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_slast, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_daddr, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_dlast, MCXNEDMAChan),
        VMSTATE_UINT32(tcd_nbytes, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_soff, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_attr, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_doff, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_citer, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_csr, MCXNEDMAChan),
        VMSTATE_UINT16(tcd_biter, MCXNEDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_mcxn_edma = {
    .name = TYPE_MCXN_EDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(mp_csr, MCXNEDMAState),
        VMSTATE_UINT32(mp_es, MCXNEDMAState),
        VMSTATE_UINT32_ARRAY(ch_grpri, MCXNEDMAState, MCXN_EDMA_CHANNELS),
        VMSTATE_STRUCT_ARRAY(ch, MCXNEDMAState, MCXN_EDMA_CHANNELS, 1,
                             vmstate_edma_chan, MCXNEDMAChan),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mcxn_edma_properties[] = {
    DEFINE_PROP_UINT8("dma-id", MCXNEDMAState, dma_id, 0),
};

static void mcxn_edma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, mcxn_edma_properties);

    dc->realize = mcxn_edma_realize;
    device_class_set_legacy_reset(dc, mcxn_edma_reset);
    dc->vmsd = &vmstate_mcxn_edma;
}

static const TypeInfo mcxn_edma_types[] = {
    {
        .name          = TYPE_MCXN_EDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNEDMAState),
        .class_init    = mcxn_edma_class_init,
    },
};

DEFINE_TYPES(mcxn_edma_types)
