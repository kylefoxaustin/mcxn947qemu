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
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/arm/boot.h"
#include "hw/arm/mcxn_soc.h"
#include "target/arm/cpu-qom.h"    /* ARM_CPU_TYPE_NAME */
/* arm_machine_interfaces: target/machine sep */
#include "hw/arm/machines-qom.h"
/* rom_ptr (read the loaded image content) */
#include "hw/core/loader.h"
#include "net/can_emu.h"
#include "qom/object.h"

/* Core clock. VERIFY against the board/RM; MCX N947 is up to 150 MHz. */
#define MCXN947_SYSCLK_HZ  150000000ULL

/*
 * QSPI execute-in-place boot: the reset image lives in the external FlexSPI NOR
 * (an 8 MiB w25q64, AHB-mapped at 0x8000_0000 non-secure and 0x9000_0000
 * secure), with the FlexSPI Config Block at offset 0x400.  A QSPI XIP image
 * links its vector table at the NOR base and the FCB at base+0x400
 * (tag "FCFB").  This model boots from the SECURE alias (0x9000_0000),
 * matching the secure internal-flash boot path.
 */
/*
 * = MCXN_FLEXSPI0_AHB_S (secure NOR alias, the addressable-as-memory XIP
 * window the loader can populate and the CPU can fetch in place)
 */
#define MCXN_QSPI_XIP_BASE  0x90000000u
#define MCXN_QSPI_XIP_SIZE  (8 * MiB)
#define MCXN_QSPI_FCB_OFF   0x400u
#define MCXN_QSPI_FCB_TAG   0x42464346u    /* "FCFB", fsl FLEXSPI_CFG_BLK_TAG */

#define TYPE_FRDM_MCXN947_MACHINE MACHINE_TYPE_NAME("frdm-mcxn947")
OBJECT_DECLARE_SIMPLE_TYPE(FrdmMcxn947Machine, FRDM_MCXN947_MACHINE)

struct FrdmMcxn947Machine {
    MachineState parent_obj;

    /*
     * Optional per-FlexCAN CAN buses for board-to-board CAN, set from the
     * command line: `-machine canbus0=<id>,canbus1=<id>` (the fleet-standard
     * incantation, matching i.MX 91/93/95).
     */
    CanBusState *canbus[MCXN_NUM_FLEXCAN];

    /*
     * `-machine qspi-boot=on`: reset from the external FlexSPI XIP NOR instead
     * of internal flash (production execute-in-place boot).
     */
    bool qspi_boot;
};

/*
 * The boot ROM's essential check: a valid FlexSPI Config Block (tag "FCFB" at
 * NOR offset 0x400) is what the ROM requires before it boots from QSPI.  Read
 * it straight from the loaded image via rom_ptr() -- the ROM blob is
 * registered by load_kernel and readable immediately (its content isn't
 * written to guest memory until the first reset, which is why a memory read
 * here would see 0).  A missing FCB is a boot failure, loud and fatal --
 * exactly what a developer whose image lacks the .flexspi_fcb section sees
 * on silicon.
 */
static void frdm_mcxn947_qspi_check_fcb(void)
{
    hwaddr fcb = MCXN_QSPI_XIP_BASE + MCXN_QSPI_FCB_OFF;
    const uint32_t *p = rom_ptr(fcb, sizeof(uint32_t));
    uint32_t tag = p ? *p : 0;

    if (tag != MCXN_QSPI_FCB_TAG) {
        error_report("frdm-mcxn947: QSPI boot image has no valid FlexSPI "
                     "Config Block at 0x%08" HWADDR_PRIx
                     " (tag 0x%08x, expected 0x%08x \"FCFB\") -- the boot "
                     "ROM would reject it.  Link the image with a "
                     ".flexspi_fcb section at offset 0x400.",
                     fcb, tag, MCXN_QSPI_FCB_TAG);
        exit(1);
    }
}

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
    /*
     * QSPI XIP boot: tell the SoC to reset from the FlexSPI NOR (init-svtor)
     * BEFORE realize.
     */
    qdev_prop_set_bit(dev, "qspi-boot", m->qspi_boot);
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

    if (m->qspi_boot) {
        /*
         * QSPI execute-in-place boot: load the image into the secure FlexSPI
         * XIP NOR window (0x9000_0000) -- the ELF loader writes each segment to
         * its linked address, and a QSPI XIP image is linked there (vectors at
         * 0x9000_0000, FCB at +0x400).  The core was told to reset from that
         * window (init-svtor above), modelling the state after the boot ROM
         * configured FlexSPI and jumped; the AHB window is a live mirror of the
         * NOR, so the reset vector fetch resolves.
         */
        armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename,
                           MCXN_QSPI_XIP_BASE, MCXN_QSPI_XIP_SIZE);
        frdm_mcxn947_qspi_check_fcb();
    } else {
        /*
         * Load firmware into the internal code-flash region. cfg is valid
         * post-realize.
         */
        armv7m_load_kernel(ARM_CPU(first_cpu), machine->kernel_filename,
                           soc->cfg->flash_base, soc->cfg->flash_size);
    }
}

/*
 * A fixed dual-Cortex-M33 part: the SoC builds the cores itself, so `-cpu`
 * is not a knob.  Advertise the one valid type so a stray `-cpu <A-core>`
 * is rejected with a clean machine-specific error instead of being silently
 * ignored (and so QEMU never pipes an incompatible core into the cluster).
 */
static const char * const frdm_mcxn947_valid_cpu_types[] = {
    ARM_CPU_TYPE_NAME("cortex-m33"),
    NULL
};

static void frdm_mcxn947_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc        = "NXP FRDM-MCXN947 (MCX N947, dual Cortex-M33)";
    mc->init        = frdm_mcxn947_init;
    mc->valid_cpu_types = frdm_mcxn947_valid_cpu_types;
    /*
     * Fixed dual-M33 part: lock the count so TCG provisions both contexts and
     * -smp can't under/over-provision (cpu0 boots, cpu1 SYSCON-released).
     */
    mc->default_cpus = MCXN_MAX_CPUS;
    mc->min_cpus     = mc->default_cpus;
    mc->max_cpus     = mc->default_cpus;
    mc->default_ram_size = 0;   /* SoC owns its memories                      */
    /* Keep transaction failures visible so peripheral stubs are loud. */
    mc->ignore_memory_transaction_failures = false;
}

static bool frdm_get_qspi_boot(Object *obj, Error **errp)
{
    return FRDM_MCXN947_MACHINE(obj)->qspi_boot;
}

static void frdm_set_qspi_boot(Object *obj, bool value, Error **errp)
{
    FRDM_MCXN947_MACHINE(obj)->qspi_boot = value;
}

static void frdm_mcxn947_machine_instance_init(Object *obj)
{
    int i;

    /*
     * Expose the per-FlexCAN `canbusN` link properties so they can be attached
     * from the command line with `-machine canbus0=<id>,canbus1=<id>`.
     */
    for (i = 0; i < MCXN_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("canbus%d", i);
        object_property_add_link(obj, name, TYPE_CAN_BUS,
                                 (Object **)
                                 &FRDM_MCXN947_MACHINE(obj)->canbus[i],
                                 object_property_allow_set_link, 0);
    }

    /* `-machine qspi-boot=on`: boot from the external FlexSPI XIP NOR. */
    object_property_add_bool(obj, "qspi-boot",
                             frdm_get_qspi_boot, frdm_set_qspi_boot);
    object_property_set_description(obj, "qspi-boot",
        "Boot (execute-in-place) from the external FlexSPI NOR (secure XIP "
        "window 0x90000000) instead of internal flash");
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
