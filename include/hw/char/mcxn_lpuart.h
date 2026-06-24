/*
 * NXP MCX N LP_FLEXCOMM in LPUART (USART) mode — console model
 *
 * Models one FlexComm instance as an LPUART.  The LPUART core registers sit at
 * the bottom of the 4 KiB block; the LP_FLEXCOMM wrapper (ISTAT @ 0xFF4,
 * PSELID @ 0xFF8) sits at the top.  Register layout is the standard NXP LPUART,
 * identical to i.MX 93/95 — an existing i.MX LPUART model ports across almost
 * unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_CHAR_MCXN_LPUART_H
#define HW_CHAR_MCXN_LPUART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_MCXN_LPUART "mcxn-lpuart"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNLPUARTState, MCXN_LPUART)

struct MCXNLPUARTState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq     irq;
    CharFrontend  chr;

    /* Register state (only what the console path needs is meaningful). */
    uint32_t global;
    uint32_t pincfg;
    uint32_t baud;
    uint32_t ctrl;
    uint32_t match;
    uint32_t modir;
    uint32_t fifo;
    uint32_t water;
    uint32_t pselid;     /* LP_FLEXCOMM peripheral-select */

    /* Extended/timeout registers the SDK driver zeroes during init (0x48..0x6C). */
    uint32_t reir;       /* Receiver Extended Idle    @0x48 */
    uint32_t teir;       /* Transmitter Extended Idle @0x4C */
    uint32_t hdcr;       /* Half Duplex Control       @0x50 */
    uint32_t tocr;       /* Timeout Control           @0x58 */
    uint32_t tosr;       /* Timeout Status            @0x5C */
    uint32_t timeout[4]; /* Timeout 0..3              @0x60..0x6C */

    uint8_t  rx_byte;
    bool     rx_full;
};

#endif /* HW_CHAR_MCXN_LPUART_H */
