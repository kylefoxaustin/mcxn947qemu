/*
 * NXP FRDM-MCXN947 reference board
 *
 * Thin board layer: provide the source clocks, instantiate the MCX N SoC with
 * the chosen part, and load the firmware image.  Everything device-specific
 * lives in the SoC.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/arm/boot.h"
#include "hw/arm/mcxn_soc.h"
#include "hw/arm/machines-qom.h"   /* DEFINE_MACHINE_ARM: target/machine separation */
#include "qom/object.h"

/* Core clock. VERIFY against the board/RM; MCX N947 is up to 150 MHz. */
#define MCXN947_SYSCLK_HZ  150000000ULL

static void frdm_mcxn947_init(MachineState *machine)
{
    MCXNState *soc;
    Clock     *sysclk, *refclk;
    DeviceState *dev;

    /* Board-supplied source clocks. */
    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, MCXN947_SYSCLK_HZ);
    refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(refclk, MCXN947_SYSCLK_HZ); /* TODO: real SysTick ref source */

    /* Instantiate the SoC. */
    soc = MCXN_SOC(object_new(TYPE_MCXN_SOC));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    object_unref(OBJECT(soc));           /* child property holds the ref now  */

    dev = DEVICE(soc);
    qdev_prop_set_string(dev, "part", "MCXN947");
    qdev_connect_clock_in(dev, "sysclk", sysclk);
    qdev_connect_clock_in(dev, "refclk", refclk);
    sysbus_realize(SYS_BUS_DEVICE(soc), &error_fatal);

    /* Load firmware into the code-flash region. cfg is valid post-realize. */
    armv7m_load_kernel(ARM_CPU(first_cpu),
                       machine->kernel_filename,
                       soc->cfg->flash_base,       /* mem_base (code flash) */
                       soc->cfg->flash_size);
}

static void frdm_mcxn947_machine_init(MachineClass *mc)
{
    mc->desc        = "NXP FRDM-MCXN947 (MCX N947, dual Cortex-M33)";
    mc->init        = frdm_mcxn947_init;
    /* Fixed dual-M33 part: lock the count so TCG provisions both contexts and
     * -smp can't under/over-provision (cpu0 boots, cpu1 SYSCON-released). */
    mc->default_cpus = MCXN_MAX_CPUS;
    mc->min_cpus     = mc->default_cpus;
    mc->max_cpus     = mc->default_cpus;
    mc->default_ram_size = 0;   /* SoC owns its memories                      */
    /* Keep transaction failures visible so peripheral stubs are loud. */
    mc->ignore_memory_transaction_failures = false;
}

DEFINE_MACHINE_ARM("frdm-mcxn947", frdm_mcxn947_machine_init)
