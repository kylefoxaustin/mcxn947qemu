/*
 * NXP MCX N eIQ Neutron NPU (neural accelerator) — behavioural model.
 *
 * The Neutron N1-16 compute block at 0x400B_E000 (RM PBRG3 map; IRQ 97 = the
 * "Reserved113" NVIC slot).  RM §20.4 states the Neutron "does not have any
 * user-configurable registers ... must be performed using NXP's eIQ Toolkit"
 * — the compute register map is NOT documented and the SDK ships the engine
 * as binary blobs (libNeutronDriver/libNeutronFirmware).  Disassembly of the
 * M33 driver gives the host-visible handshake:
 *
 *   - CTRL  @ 0x00: write kicks a microcode step; bit31 = SHADOW_BUSY.
 *       neutron_exec() spins  while ((int32)CTRL < 0)  [bit31 busy]
 *       neutron_done() spins  while (CTRL != 0)        [fully idle = done]
 *   - INTR  @ 0x40: INTREN b0, EVENTEN b1, ERRORTRAP_M b2, ERRORTRAP_R b3
 *       (init writes EVENTEN; the eIQ completion path uses the CPU EVENT/WFE,
 *        not NVIC IRQ 97).
 *
 * We cannot run the proprietary Neutron microcode, so this is a
 * FLAG-AT-OPERATOR block: CTRL goes idle synchronously on write so the
 * exec/done spins never hang, but the model NEVER claims to have computed —
 * the inference result is left uncomputed and that truth is exposed to the farm
 * control-plane via QMP (`compute-modelled`=false + `jobs-started`) and a
 * LOG_UNIMP per kick, so a silently-wrong inference is detectable, never
 * hidden.  An operator may opt in to surface the fault to the guest too
 * (`uncomputed-errortrap`) via the non-gating INTR.ERRORTRAP bit + NPU IRQ 97
 * — never via the completion gate.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_MCXN_NEUTRON_H
#define HW_MISC_MCXN_NEUTRON_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MCXN_NEUTRON "mcxn-neutron"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNNeutronState, MCXN_NEUTRON)

#define MCXN_NEUTRON_SIZE 0x1000

struct MCXNNeutronState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[MCXN_NEUTRON_SIZE / 4];

    uint32_t jobs_started;          /* acked-but-uncomputed CTRL kicks       */
    bool     uncomputed_errortrap;  /* operator opt-in: fault the guest too  */
};

#endif /* HW_MISC_MCXN_NEUTRON_H */
