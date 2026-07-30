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
#define MCXN_INPUTMUX_NDAC      3   /* DAC0/1/2, one trigger selector each */
#define MCXN_INPUTMUX_NCMP      3   /* CMP0/1/2, one trigger selector each */
#define MCXN_INPUTMUX_NQDC      2   /* QDC0/1, one trigger selector each */
#define MCXN_INPUTMUX_NTRIG_SRC 128   /* selector values we can route */

/*
 * Trigger source selector numbers.  DERIVED, NOT INVENTED: decoded from NXP's own
 * compiled SDK driver, where kINPUTMUX_Lptmr0ToAdc0Trigger = 0x2800_0032 and
 * INPUTMUX_AttachSignal(base, idx, conn) does
 *     *(base + (conn >> 20) + idx * 4) = conn & 0xFFFFF
 * -> "write 50 (0x32) into the register at offset 0x280", which is ADC0_TRIG[0].
 */
#define MCXN_INPUTMUX_SRC_LPTMR0 50
/*
 * CTIMER{k} match-3 -> ADCn trigger.  kINPUTMUX_Ctimer{k}M3ToAdc0Trigger = 5+k in
 * NXP's driver.  CTIMER0/1/2 use M3 for BOTH ADC0 and ADC1 (collision-free in this
 * shared-selector model); CTIMER3/4 diverge on ADC1 (M2/M1) -- a stated boundary.
 */
#define MCXN_INPUTMUX_SRC_CTIMER0_M3 5
#define MCXN_INPUTMUX_SRC_CTIMER1_M3 6
#define MCXN_INPUTMUX_SRC_CTIMER2_M3 7
/*
 * CTIMER3/4 -> ADC selectors 8/9 are PER-DESTINATION: ADC0_TRIG=8/9 name Ctimer3M3 /
 * Ctimer4M3, but ADC1_TRIG=8/9 name Ctimer3M2 / Ctimer4M1 (NXP's connection table).
 * The shared trig-in source space cannot use the raw selector for both, so the two
 * ADC1-facing match lines get distinct source ids (>65, clear of every selector value)
 * and the router remaps ADC1's selectors 8/9 to them (mcxn_inputmux_trigger).
 */
#define MCXN_INPUTMUX_SRC_CTIMER3_M3 8    /* ADC0 selector 8 (identity) */
#define MCXN_INPUTMUX_SRC_CTIMER4_M3 9    /* ADC0 selector 9 (identity) */
#define MCXN_INPUTMUX_SRC_CTIMER3_M2 96   /* ADC1 selector 8 remaps here */
#define MCXN_INPUTMUX_SRC_CTIMER4_M1 97   /* ADC1 selector 9 remaps here */
/*
 * eFlexPWM submodule-0 output triggers -> ADCn trigger.  kINPUTMUX_Pwm{m}A0Trig{t}ToAdc0
 * = 24 + 8*m + t in NXP's driver (submodule-0 = "A0"; the model runs SM0 only).  These
 * are the motor-control synchronous-sampling path: a VALn compare fires PWM_OUT_TRIGt,
 * which the ADC samples phase-current on, mid-carrier.
 */
#define MCXN_INPUTMUX_SRC_PWM0_SM0_TRIG0 24
#define MCXN_INPUTMUX_SRC_PWM0_SM0_TRIG1 25
#define MCXN_INPUTMUX_SRC_PWM1_SM0_TRIG0 32
#define MCXN_INPUTMUX_SRC_PWM1_SM0_TRIG1 33

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
    qemu_irq dac_trig[MCXN_INPUTMUX_NDAC];   /* DAC0/1/2 hardware trigger */
    qemu_irq cmp_trig[MCXN_INPUTMUX_NCMP];   /* CMP0/1/2 round-robin trigger */
    qemu_irq qdc_trig[MCXN_INPUTMUX_NQDC];   /* QDC0/1 position-capture trigger */
};

#endif /* HW_MISC_MCXN_INPUTMUX_H */
