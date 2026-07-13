/*
 * NXP MCX N INPUTMUX (input multiplexer / trigger routing) — bring-up model.
 *
 * Pure register-routing block: selects which signals drive peripheral
 * triggers, capture inputs, DMA requests, etc.  Faithful register-backed
 * model (all registers reset to 0).  Routing has no externally-observable
 * effect in emulation.  Offsets from the MCXN947 CMSIS header (INPUTMUX_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_INPUTMUX_H
#define HW_MISC_MCXN_INPUTMUX_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_INPUTMUX "mcxn-inputmux"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNInputMuxState, MCXN_INPUTMUX)

#define MCXN_INPUTMUX_SIZE 0x1000

/* DMA0/DMA1, 128 request sources each (DMAn_REQ_ENABLE0..3, 32 bits apiece). */
#define MCXN_INPUTMUX_NDMA 2
#define MCXN_INPUTMUX_NREQ 128

/* ADC0_TRIG[4] @0x280, ADC1_TRIG[4] @0x2C0 (CMSIS INPUTMUX_Type). */
#define MCXN_INPUTMUX_NADC      2
#define MCXN_INPUTMUX_NADC_TRIG 4
#define MCXN_INPUTMUX_NTRIG_SRC 128   /* selector values we can route */

/*
 * Trigger source selector numbers.  DERIVED, NOT INVENTED: decoded from NXP's own
 * compiled SDK driver, where kINPUTMUX_Lptmr0ToAdc0Trigger = 0x2800_0032 and
 * INPUTMUX_AttachSignal(base, idx, conn) does
 *     *(base + (conn >> 20) + idx * 4) = conn & 0xFFFFF
 * -> "write 50 (0x32) into the register at offset 0x280", which is ADC0_TRIG[0].
 */
#define MCXN_INPUTMUX_SRC_LPTMR0 50

struct MCXNInputMuxState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t regs[MCXN_INPUTMUX_SIZE / 4];

    /*
     * One output line per eDMA request source, per eDMA.  These carry
     * DMAn_REQ_ENABLE0..3 -- the gate that decides whether a peripheral's DMA
     * request is allowed to reach the engine at all.
     *
     * ⚠ Until now this file's own comment said INPUTMUX registers "have no
     * externally-observable behavior in emulation, so storing and reading back the
     * written value is faithful."  THAT JUSTIFICATION IS CIRCULAR: they had no
     * observable behaviour ONLY BECAUSE NOTHING WAS CONNECTED TO THE OTHER END.
     * The stub asserted its own irrelevance, and the assertion was self-fulfilling.
     */
    qemu_irq dma_req_enable[MCXN_INPUTMUX_NDMA][MCXN_INPUTMUX_NREQ];

    /*
     * TRIGGER ROUTING.  ADCn_TRIG[0..3] is a SELECTOR: it names which trigger source
     * drives that ADC trigger input.  One input line per possible selector value, one
     * output line per ADC trigger input; a pulse on source S is forwarded to every
     * destination whose selector currently reads S.
     *
     * Selector 50 = LPTMR0.  That number is not a guess: it was decoded from NXP's
     * own compiled driver, where kINPUTMUX_Lptmr0ToAdc0Trigger = 0x2800_0032 and
     * INPUTMUX_AttachSignal() does  *(base + (conn>>20) + idx*4) = conn & 0xFFFFF,
     * i.e. "write 50 into the register at offset 0x280".
     */
    qemu_irq adc_trig[MCXN_INPUTMUX_NADC][MCXN_INPUTMUX_NADC_TRIG];
};

#endif /* HW_MISC_MCXN_INPUTMUX_H */
