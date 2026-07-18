/*
 * NXP MCX N FlexSPI — functional controller driving a real SPI-NOR.
 *
 * The FRDM-MCXN947 hangs an 8 MiB Winbond W25Q64 off FlexSPI0, mapped into the
 * AHB window (NS 0x8000_0000 / secure 0x9000_0000).  Two constraints are in
 * tension, and both must hold:
 *
 *   1. The flash CONTENT and its PHYSICS must be real.  QEMU already models this
 *      part — m25p80 "w25q64", JEDEC 0xef4017, the exact silicon on the board —
 *      and it enforces erase-before-write, bits only 1 -> 0, page-program wrap
 *      and the WREN latch.  So we attach it on an SSI bus and let the LUT/IP
 *      command path drive it, rather than hand-rolling the NOR semantics.
 *
 *   2. The AHB window must be EXECUTABLE, because the MCX genuinely runs code in
 *      place from it (tests/mcxn-xip).  An SSI device cannot be executed from:
 *      QEMU *can* fetch instructions through an MMIO region, but it refuses to
 *      cache the translation block (accel/tcg/translator.c), so XIP through an
 *      io window still works and is ~100x slower — a regression that reports
 *      success and is only visible on a wall clock.
 *
 * So the window is a ROM *device* holding a MIRROR of the flash, and:
 *
 *   - reads and instruction fetch hit the mirror directly (fast, TCG-cacheable);
 *   - stores into the window are REFUSED (a CPU store does not program a NOR);
 *   - m25p80 is the SOLE AUTHORITY for content.  The mirror is strictly derived:
 *     re-synced from the flash after any IP command that changes it, and then
 *     published with memory_region_flush_rom_device() so TCG drops translation
 *     blocks for code that was just reprogrammed.
 *
 * The single-authority rule is load-bearing, and getting it wrong would create a
 * new silent-wrong while fixing one: if the mirror could be written without the
 * flash agreeing, the two would diverge and the next erase would silently
 * resurrect stale content underneath a running image.  Hence also the -kernel
 * case: an image linked into the XIP window is loaded by QEMU's ROM loader
 * straight into the mirror (address_space_write_rom bypasses the write op), so
 * before the first IP command we PROGRAM IT INTO THE NOR — because that is what
 * it means on hardware.  The firmware is *in* the flash.
 *
 * (The i.MX 93/95 siblings attach m25p80 with a plain init_io window.  That is
 * correct where flash is data-only; it cannot execute in place.  Design refined
 * with rt1180emulator, who is the other XIP-capable node.)
 *
 * Offsets/bits from the MCXN947 CMSIS header (FLEXSPI_Type); the LUT instruction
 * encoding from the MCUXpresso SDK (fsl_flexspi.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FLEXSPI_H
#define HW_MISC_MCXN_FLEXSPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

/* CMSIS FLEXSPI_Type spans up to 0x5F4; round the window to 0x1000. */
#define MCXN_FLEXSPI_SIZE 0x1000

/* W25Q64 geometry (m25p80 "w25q64"). */
#define MCXN_NOR_PAGE     256
#define MCXN_NOR_SECTOR   0x1000     /* 4 KiB  */
#define MCXN_NOR_BLOCK32  0x8000     /* 32 KiB */
#define MCXN_NOR_BLOCK64  0x10000    /* 64 KiB */

/* Largest IP transfer we buffer in one command. */
#define MCXN_FLEXSPI_XFER_MAX 4096

#define TYPE_MCXN_FLEXSPI "mcxn-flexspi"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNFlexSPIState, MCXN_FLEXSPI)

struct MCXNFlexSPIState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;        /* MMIO region 0: CMSIS register file           */
    MemoryRegion nor;          /* MMIO region 1: AHB XIP mirror (ROM device)   */
    qemu_irq irq;
    /* eDMA request lines (CMSIS FlexSPI0 Rx=1, Tx=2).  A DMA-driven FlexSPI driver
     * (FLEXSPI_TransferEDMA) arms a channel at RFDR/TFDR and sets IP{RX,TX}FCR[DMAEN];
     * without these the channel waits forever and the transfer never runs. */
    qemu_irq dma_req_rx;
    qemu_irq dma_req_tx;
    bool     rx_dma_lvl;       /* last level driven on each line (dedupe) */
    bool     tx_dma_lvl;
    uint32_t regs[MCXN_FLEXSPI_SIZE / 4];
    uint64_t flash_size;       /* size of the AHB NOR window ("flash-size")    */

    /* The real flash: m25p80 on our SSI bus.  Sole authority for content. */
    SSIBus  *spi;
    qemu_irq cs;               /* active low */

    /* An -kernel XIP image lands in the mirror; push it into the NOR before the
     * first IP command so the flash, not the mirror, remains authoritative. */
    bool     loader_flushed;

    /* IP command data path. */
    uint8_t  rx_buf[MCXN_FLEXSPI_XFER_MAX];
    uint32_t rx_len;           /* bytes produced by the last read command      */
    uint32_t rx_pos;           /* bytes already popped by the guest            */
    uint32_t tx_len;           /* bytes streamed by the guest via TFDR         */

    /* A program command in flight: CS stays asserted while the guest feeds TFDR. */
    bool     pgm_pending;
    uint32_t pgm_addr;
    uint32_t pgm_len;
};

#endif /* HW_MISC_MCXN_FLEXSPI_H */
