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
#include "hw/arm/machines-qom.h"   /* arm_machine_interfaces: target/machine sep */
#include "net/can_emu.h"
#include "qom/object.h"

/* Core clock. VERIFY against the board/RM; MCX N947 is up to 150 MHz. */
#define MCXN947_SYSCLK_HZ  150000000ULL

#define TYPE_FRDM_MCXN947_MACHINE MACHINE_TYPE_NAME("frdm-mcxn947")
OBJECT_DECLARE_SIMPLE_TYPE(FrdmMcxn947Machine, FRDM_MCXN947_MACHINE)

struct FrdmMcxn947Machine {
    MachineState parent_obj;

    /* Optional per-FlexCAN CAN buses for board-to-board CAN, set from the
     * command line: `-machine canbus0=<id>,canbus1=<id>` (the fleet-standard
     * incantation, matching i.MX 91/93/95). */
    CanBusState *canbus[MCXN_NUM_FLEXCAN];
};

static void frdm_mcxn947_init(MachineState *machine)
{
    FrdmMcxn947Machine *m = FRDM_MCXN947_MACHINE(machine);
    MCXNState *soc;
    Clock     *sysclk, *refclk;
    DeviceState *dev;
    int i;

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

    /*
     * Board-to-board CAN: forward each FlexCAN's CAN bus to the SoC before
     * realize.  Prefer the `-machine canbusN=<id>` link (the fleet-standard,
     * same as i.MX 91/93/95); as a convenience fall back to a command-line
     * CAN-bus object simply *named* `canbusN` (`-object can-bus,id=canbus0`).
     * Absent = the FlexCAN stays loopback-only.
     */
    for (i = 0; i < MCXN_NUM_FLEXCAN; i++) {
        g_autofree char *id = g_strdup_printf("canbus%d", i);
        Object *cb = OBJECT(m->canbus[i]);

        if (!cb) {
            cb = object_resolve_path_component(object_get_objects_root(), id);
        }
        if (cb) {
            object_property_set_link(OBJECT(soc), id, cb, &error_fatal);
        }
    }

    sysbus_realize(SYS_BUS_DEVICE(soc), &error_fatal);

    /* Load firmware into the code-flash region. cfg is valid post-realize. */
    armv7m_load_kernel(ARM_CPU(first_cpu),
                       machine->kernel_filename,
                       soc->cfg->flash_base,       /* mem_base (code flash) */
                       soc->cfg->flash_size);
}

static void frdm_mcxn947_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

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

static void frdm_mcxn947_machine_instance_init(Object *obj)
{
    int i;

    /* Expose the per-FlexCAN `canbusN` link properties so they can be attached
     * from the command line with `-machine canbus0=<id>,canbus1=<id>`. */
    for (i = 0; i < MCXN_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("canbus%d", i);
        object_property_add_link(obj, name, TYPE_CAN_BUS,
                                 (Object **)&FRDM_MCXN947_MACHINE(obj)->canbus[i],
                                 object_property_allow_set_link, 0);
    }
}

static const TypeInfo frdm_mcxn947_machine_types[] = {
    {
        .name          = TYPE_FRDM_MCXN947_MACHINE,
        .parent        = TYPE_MACHINE,
        .class_init    = frdm_mcxn947_machine_class_init,
        .instance_init = frdm_mcxn947_machine_instance_init,
        .instance_size = sizeof(FrdmMcxn947Machine),
        .interfaces    = arm_machine_interfaces,
    },
};

DEFINE_TYPES(frdm_mcxn947_machine_types)
