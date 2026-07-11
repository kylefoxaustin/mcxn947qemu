/*
 * NXP MCX N FMU (Flash Management Unit) — functional flash controller.
 *
 * Models the real MCX N program/erase protocol, which is NOT a "data in FCCOB"
 * design (RM rev 7 §8.3.2.11 / §8.3.2.12 / §8.3.2.14).  Program Page, Program
 * Phrase and Erase Sector all use a PEWEN/PERDY handshake in which the target
 * address and the data arrive as ordinary CPU stores into the flash address
 * space, and those stores only take effect while the controller enables them:
 *
 *   1. FCCOB0 = command; launch by writing 1 to FSTAT[CCIF] (W1C -> busy).
 *   2. The controller sets FSTAT[PEWEN] (01 = one phrase, 10 = one page).
 *   3. Firmware writes N consecutive words into the flash address space; the
 *      first store must be phrase/page aligned.  This is how the controller
 *      learns the address.  The data is not visible to reads yet.
 *   4. On the Nth word the controller clears PEWEN, sets FSTAT[PERDY], stalls.
 *   5. Firmware clears PERDY (W1C); the controller commits and sets CCIF.
 *
 * Consequences modelled faithfully, because getting them wrong is exactly the
 * silent-wrong-answer class this project exists to catch:
 *
 *   - A store into flash space *outside* an enabled program/erase window does
 *     nothing (on silicon it is trapped outside the flash module).  Firmware
 *     that scribbles at flash addresses must not appear to work.
 *   - Flash programs bits 1 -> 0 only.  A program commits (old & new), and the
 *     verify sets FSTAT[FAIL] if a bit asked to stay 1 came back 0.  That makes
 *     cumulative programming without an intervening erase fail, as the RM
 *     requires (§8.3.2.11 CAUTION).
 *   - Erase Sector takes its target from the phrase written during PEWEN, not
 *     from FCCOB.
 *
 * The Read 1s (verify-erased) commands DO take an address, and it lives in
 * FCCOB2 — not FCCOB1, which is command options (§8.3.1.1.1).
 *
 * Geometry (RM §8.1): phrase = 16 B, page = 128 B, sector = 8 KB.
 * Offsets/bits from the MCXN947 CMSIS header (FMU_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_FMU_H
#define HW_MISC_MCXN_FMU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/memory.h"

#define TYPE_MCXN_FMU "mcxn-fmu"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNFMUState, MCXN_FMU)

#define MCXN_FLASH_PHRASE 16      /* smallest programmable unit   */
#define MCXN_FLASH_PAGE   128     /* largest programmable unit    */
#define MCXN_FLASH_SECTOR 0x2000  /* smallest erasable unit, 8 KB */

struct MCXNFMUState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;

    uint32_t fstat;
    uint32_t fcnfg;
    uint32_t fctrl;
    uint32_t fccob[8];

    /* The program/erase write window (the PEWEN..PERDY phase). */
    uint8_t  pe_buf[MCXN_FLASH_PAGE]; /* words staged by the guest's stores  */
    uint64_t pe_addr;                 /* flash offset of the first store     */
    uint32_t pe_words;                /* words staged so far                 */
    uint32_t pe_expect;               /* words this command wants (4 or 32)  */
    uint8_t  pe_cmd;                  /* command awaiting its write phase    */
    bool     pe_active;               /* a write window is open              */

    /* Backing flash: a ROM-device region owned by the FMU, so that guest
     * stores land in the FMU instead of silently succeeding as if it were RAM. */
    MemoryRegion *flash;
    uint64_t      flash_size;
};

/*
 * Create the flash region as a ROM device whose writes are gated by the FMU.
 * Reads and instruction fetch go straight to the backing RAM (so XIP and the
 * -kernel ROM loader are unaffected); stores are routed into the FMU.
 */
void mcxn_fmu_init_flash(MCXNFMUState *s, Object *owner, MemoryRegion *flash,
                         uint64_t size, Error **errp);

#endif /* HW_MISC_MCXN_FMU_H */
