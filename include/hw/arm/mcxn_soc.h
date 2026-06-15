/*
 * NXP MCX N-series SoC (Arm Cortex-M33)
 *
 * Part-agnostic SoC container.  Adding a new MCX variant is a single table
 * entry in mcxn_soc.c (see MCXNConfig); the rest of the model is generic.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_MCXN_SOC_H
#define HW_ARM_MCXN_SOC_H

#include "hw/core/sysbus.h"
#include "hw/arm/armv7m.h"
#include "hw/char/mcxn_lpuart.h"
#include "hw/misc/mcxn_scg.h"
#include "hw/misc/mcxn_syscon.h"
#include "hw/misc/mcxn_port.h"
#include "hw/gpio/mcxn_gpio.h"
#include "hw/core/clock.h"
#include "qom/object.h"

#define TYPE_MCXN_SOC "mcxn-soc"
OBJECT_DECLARE_SIMPLE_TYPE(MCXNState, MCXN_SOC)

/* Max Cortex-M33 cores across the MCX N family (MCXN947 = 2). */
#define MCXN_MAX_CPUS 2

/* MCXN947 GPIO/PORT instance counts (GPIO0..5, PORT0..5). */
#define MCXN_NUM_GPIO 6
#define MCXN_NUM_PORT 6

/*
 * Per-SKU configuration.
 *
 * Everything below the CPU line is SKU-specific and MUST be confirmed against
 * the part Reference Manual before being trusted.  The Cortex-M33
 * architectural layout (code @ 0x0, SRAM @ 0x2000_0000, peripheral window
 * @ 0x4000_0000, PPB @ 0xE000_0000) is fixed by the architecture and is either
 * set here or handled inside the ARMV7M container.
 */
typedef struct MCXNConfig {
    const char *name;          /* e.g. "MCXN947"                              */
    const char *cpu_type;      /* ARM_CPU_TYPE_NAME("cortex-m33")             */
    uint32_t    num_cpus;      /* MVP wires cpu0 only; dual-core is follow-on */
    uint32_t    num_irq;       /* NVIC external IRQ lines  (VERIFY: RM)       */
    uint8_t     num_prio_bits; /* __NVIC_PRIO_BITS         (VERIFY: RM)       */
    hwaddr      flash_base;
    uint64_t    flash_size;
    hwaddr      sram_base;
    uint64_t    sram_size;
} MCXNConfig;

struct MCXNState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    ARMv7MState  armv7m[MCXN_MAX_CPUS];   /* dual Cortex-M33 (cpu0 + cpu1) */
    MemoryRegion cpu_mem[MCXN_MAX_CPUS];  /* per-core alias view of the SoC map */
    MCXNLPUARTState flexcomm4;     /* FRDM debug console (LPUART4) */
    MCXNSCGState    scg0;          /* system clock generator (stub) */
    MCXNSysconState syscon;        /* CPU1 boot control (CPUCTRL/CPBOOT) */
    MCXNGPIOState   gpio[MCXN_NUM_GPIO];  /* GPIO0..5 controllers */
    MCXNPortState   port[MCXN_NUM_PORT];  /* PORT0..5 pin-mux stubs */
    Clock      *sysclk;
    Clock      *refclk;

    MemoryRegion flash;
    MemoryRegion sram;
    MemoryRegion flexcomm4_s_alias; /* TrustZone secure alias of the console */
    MemoryRegion scg0_s_alias;      /* TrustZone secure alias of SCG0 */
    MemoryRegion syscon_s_alias;    /* TrustZone secure alias of SYSCON */
    MemoryRegion gpio_s_alias[MCXN_NUM_GPIO]; /* secure aliases of GPIO0..5 */
    MemoryRegion port_s_alias[MCXN_NUM_PORT]; /* secure aliases of PORT0..5 */

    const MCXNConfig *cfg;      /* resolved from "part" at realize time       */
    char            *part;      /* settable property: selects the MCXNConfig  */
};

#endif /* HW_ARM_MCXN_SOC_H */
