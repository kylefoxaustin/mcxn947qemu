/*
 * NXP MCX N FlexSPI (Flexible Serial Peripheral Interface) — functional model
 * with a real SPI-NOR behind it.
 *
 * The FRDM-MCXN947 hangs an 8 MiB Winbond W25Q64 NOR off FlexSPI0, mapped into
 * the AHB window (NS 0x8000_0000 / secure 0x9000_0000).  Two things must both be
 * true, and the bring-up model got the second one wrong:
 *
 *   1. The AHB window must be executable, because the MCX genuinely runs code in
 *      place from it (tests/mcxn-xip).  So it is a ROM *device*: reads and
 *      instruction fetch go straight to the backing array, and the -kernel ROM
 *      loader fills it.
 *
 *   2. A CPU store into that window must NOT program the flash.  On silicon a
 *      NOR is programmed only by an erase + page-program command sequence issued
 *      through the controller's LUT/IP path — a plain store does nothing.  The
 *      bring-up model backed the window with memory_region_init_ram(), so every
 *      store simply landed: firmware that scribbled at XIP addresses "worked",
 *      and the entire IP command path could stay a stub without anyone noticing.
 *      That is the same silent-wrong the internal flash (FMU) had, and it is why
 *      the fleet's rule is: never back a flash region with init_ram.
 *
 * So writes into the AHB window are routed here and refused (loudly), and the IP
 * command path is real: LUT sequences are decoded, and the SPI-NOR opcodes they
 * carry are executed against the backing array with NOR physics —
 *
 *   - program clears bits only (array &= data), so programming without an erase
 *     corrupts exactly as it does on silicon (a NOR raises no error for this,
 *     which is precisely why it must not silently succeed here);
 *   - a page program wraps within its 256-byte page;
 *   - program/erase require the write-enable latch (WREN), which self-clears.
 *
 * The i.MX 93/95 siblings get these physics for free by attaching QEMU's m25p80
 * SSI NOR to their FlexSPI.  That is the right answer when flash is data-only,
 * but it cannot execute in place — TCG cannot fetch instructions through an SSI
 * device — so an XIP-capable MCU needs the ROM-device shape used here instead.
 *
 * Offsets/bits from the MCXN947 CMSIS header (FLEXSPI_Type) and the LUT
 * instruction encoding from the MCUXpresso SDK (fsl_flexspi.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FLEXSPI_H
#define HW_MISC_MCXN_FLEXSPI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

/* CMSIS FLEXSPI_Type spans up to 0x5F4; round the window to 0x1000. */
#define MCXN_FLEXSPI_SIZE 0x1000

/* W25Q64 geometry. */
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
    MemoryRegion nor;          /* MMIO region 1: AHB NOR window (ROM device)   */
    qemu_irq irq;
    uint32_t regs[MCXN_FLEXSPI_SIZE / 4];
    uint64_t flash_size;       /* size of the AHB NOR window ("flash-size")    */

    /* NOR device state. */
    bool     wel;              /* write-enable latch (WREN sets, op clears)    */

    /* IP command data path. */
    uint8_t  rx_buf[MCXN_FLEXSPI_XFER_MAX];
    uint32_t rx_len;           /* bytes produced by the last read command      */
    uint32_t rx_pos;           /* bytes already popped by the guest            */
    uint8_t  tx_buf[MCXN_FLEXSPI_XFER_MAX];
    uint32_t tx_len;           /* bytes staged by the guest via TFDR           */

    /* A program command in flight, waiting for the guest to feed TFDR. */
    bool     pgm_pending;
    uint32_t pgm_addr;
    uint32_t pgm_len;
};

#endif /* HW_MISC_MCXN_FLEXSPI_H */
