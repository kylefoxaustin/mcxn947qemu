/*
 * NXP MCX N-series SoC (Arm Cortex-M33)
 *
 * Bring-up strategy: instantiate the ARMV7M container (CPU + NVIC + SysTick),
 * map code/SRAM, and cover the entire peripheral window with a single
 * "unimplemented" catch-all so that running real firmware logs exactly which
 * peripherals are touched (and in what order).  You then peel real device
 * models out of the catch-all one at a time.  This is deliberately
 * part-agnostic: only the MCXNConfig table below is SKU-specific.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/mcxn_soc.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h" /* qdev_prop_set_chr */
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"             /* serial_hd (older trees: sysemu/sysemu.h) */
#include "target/arm/cpu-qom.h" /* ARM_CPU_TYPE_NAME */

/* FRDM-MCXN947 debug console: FlexComm4 / LPUART4 (NS alias) + its NVIC line. */
#define MCXN_FLEXCOMM4_BASE  0x400B4000
#define MCXN_FLEXCOMM4_IRQ   39          /* CMSIS: LP_FLEXCOMM4_IRQn */
#define MCXN_SCG0_BASE       0x40044000  /* system clock generator (NS alias) */
#define MCXN_SYSCON_BASE     0x40000000  /* SYSCON (NS alias); CPU1 boot ctrl */
#define MCXN_SPC0_BASE       0x40045000  /* system power controller (NS alias) */

/* SRAM is reachable on two buses; SRAMX is a separate code-bus RAM. */
#define MCXN_SRAM_CODEBUS    0x30000000  /* code-bus alias of the system SRAM */
#define MCXN_SRAMX_BASE      0x14000000
#define MCXN_SRAMX_SIZE      (96 * KiB)

/* TrustZone-M: secure peripheral alias = non-secure base + 0x1000_0000. */
#define MCXN_SECURE_ALIAS    0x10000000

/* ------------------------------------------------------------------------- *
 *  Per-SKU table.  Add new MCX variants here; nothing else needs to change.
 *
 *  MCXN947 values verified against the CMSIS device header
 *  (mcux-sdk: devices/MCXN947/MCXN947_cm33_core0.h):
 *    - highest IRQ = CTI0_IRQn (155)  -> num_irq = 156
 *    - __NVIC_PRIO_BITS = 3
 *    - M33 with FPU + DSP + MPU + SAU/TrustZone-M
 *  Memory bases verified against the NXP frdm_mcxn947 Zephyr linker: code
 *  flash is on the code bus at 0x1000_0000 (NOT the generic M-profile 0x0),
 *  the 512 KiB SRAM has a system-bus view at 0x2000_0000 and a code-bus alias
 *  at 0x3000_0000 (the view Zephyr links its RAM to), and SRAMX (96 KiB) sits
 *  at 0x1400_0000.  cpu reset reads its vector table from flash (init-svtor).
 * ------------------------------------------------------------------------- */
static const MCXNConfig mcxn_configs[] = {
    {
        .name          = "MCXN947",
        .cpu_type      = ARM_CPU_TYPE_NAME("cortex-m33"),
        .num_cpus      = 2,            /* dual Cortex-M33 (cpu0 + cpu1)        */
        .num_irq       = 156,          /* CMSIS: CTI0_IRQn=155, +1             */
        .num_prio_bits = 3,            /* CMSIS: __NVIC_PRIO_BITS              */
        .flash_base    = 0x10000000,   /* code-bus flash (Zephyr boots here)  */
        .flash_size    = 2 * MiB,
        .sram_base     = 0x20000000,   /* SRAM system-bus view                */
        .sram_size     = 512 * KiB,
    },
    /* Add MCX N54x / N23x / A-series / W-series entries here. */
};

/* GPIO0..5 / PORT0..5 NS base addresses (CMSIS).  GPIO0..4 and PORT0..4 are on
 * a regular stride; GPIO5/PORT5 sit in a separate aliased block. */
static const hwaddr mcxn_gpio_base[MCXN_NUM_GPIO] = {
    0x40096000, 0x40098000, 0x4009A000, 0x4009C000, 0x4009E000, 0x40040000,
};
static const hwaddr mcxn_port_base[MCXN_NUM_PORT] = {
    0x40116000, 0x40117000, 0x40118000, 0x40119000, 0x4011A000, 0x40042000,
};

static const MCXNConfig *mcxn_lookup(const char *part)
{
    int i;
    for (i = 0; i < ARRAY_SIZE(mcxn_configs); i++) {
        if (!strcmp(part, mcxn_configs[i].name)) {
            return &mcxn_configs[i];
        }
    }
    return NULL;
}

static void mcxn_soc_instance_init(Object *obj)
{
    MCXNState *s = MCXN_SOC(obj);
    int i;

    for (i = 0; i < MCXN_MAX_CPUS; i++) {
        g_autofree char *name = g_strdup_printf("cpu%d", i);
        object_initialize_child(obj, name, &s->armv7m[i], TYPE_ARMV7M);
    }
    object_initialize_child(obj, "flexcomm4", &s->flexcomm4, TYPE_MCXN_LPUART);
    object_initialize_child(obj, "scg0", &s->scg0, TYPE_MCXN_SCG);
    object_initialize_child(obj, "syscon", &s->syscon, TYPE_MCXN_SYSCON);
    object_initialize_child(obj, "spc0", &s->spc0, TYPE_MCXN_SPC);
    for (i = 0; i < MCXN_NUM_GPIO; i++) {
        g_autofree char *name = g_strdup_printf("gpio%d", i);
        object_initialize_child(obj, name, &s->gpio[i], TYPE_MCXN_GPIO);
    }
    for (i = 0; i < MCXN_NUM_PORT; i++) {
        g_autofree char *name = g_strdup_printf("port%d", i);
        object_initialize_child(obj, name, &s->port[i], TYPE_MCXN_PORT);
    }

    /* Input clocks the board drives; forwarded to the ARMV7M container. */
    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->refclk = qdev_init_clock_in(DEVICE(s), "refclk", NULL, NULL, 0);
}

static void mcxn_soc_realize(DeviceState *dev, Error **errp)
{
    MCXNState     *s             = MCXN_SOC(dev);
    MemoryRegion  *system_memory = get_system_memory();
    const MCXNConfig *cfg;
    uint32_t ncpu;
    int i;

    cfg = mcxn_lookup(s->part ? s->part : "MCXN947");
    if (!cfg) {
        error_setg(errp, "mcxn-soc: unknown part '%s'", s->part);
        return;
    }
    s->cfg = cfg;

    /* --- Memories -------------------------------------------------------- *
     * Code flash is modelled as RAM during bring-up so the loader can write
     * it directly.  Swap to memory_region_init_rom() + a flash controller
     * model once the boot/flash path is being exercised for real.
     */
    memory_region_init_ram(&s->flash, OBJECT(dev), "mcxn.flash",
                           cfg->flash_size, &error_fatal);
    memory_region_add_subregion(system_memory, cfg->flash_base, &s->flash);

    memory_region_init_ram(&s->sram, OBJECT(dev), "mcxn.sram",
                           cfg->sram_size, &error_fatal);
    memory_region_add_subregion(system_memory, cfg->sram_base, &s->sram);

    /* Same SRAM, code-bus view at 0x3000_0000 (Zephyr links its RAM here). */
    memory_region_init_alias(&s->sram_codebus, OBJECT(dev), "mcxn.sram.codebus",
                             &s->sram, 0, cfg->sram_size);
    memory_region_add_subregion(system_memory, MCXN_SRAM_CODEBUS,
                                &s->sram_codebus);

    /* SRAMX: separate code-bus RAM. */
    memory_region_init_ram(&s->sramx, OBJECT(dev), "mcxn.sramx",
                           MCXN_SRAMX_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, MCXN_SRAMX_BASE, &s->sramx);

    /* --- Cortex-M33 cores + NVIC + SysTick ------------------------------- *
     * The MCXN947 is a dual-M33 part.  Each ARMV7M wraps its "memory" link in
     * its own private per-core container (with that core's NVIC/SysTick/PPB),
     * so the two cores cannot share a single MemoryRegion directly — each gets
     * its OWN alias of the shared SoC map instead.  cpu0 is the primary boot
     * core; cpu1 starts held in reset (start-powered-off) and is released at
     * runtime by cpu0 firmware via the SYSCON CPUCTRL/CPBOOT block (see
     * mcxn_syscon).
     */
    ncpu = cfg->num_cpus ? cfg->num_cpus : 1;
    if (ncpu > MCXN_MAX_CPUS) {
        ncpu = MCXN_MAX_CPUS;
    }
    for (i = 0; i < ncpu; i++) {
        DeviceState *cpudev = DEVICE(&s->armv7m[i]);
        g_autofree char *view = g_strdup_printf("mcxn-cpu%d-view", i);

        memory_region_init_alias(&s->cpu_mem[i], OBJECT(dev), view,
                                 system_memory, 0, UINT64_MAX);

        qdev_prop_set_uint32(cpudev, "num-irq",       cfg->num_irq);
        qdev_prop_set_uint8 (cpudev, "num-prio-bits", cfg->num_prio_bits);
        qdev_prop_set_string(cpudev, "cpu-type",      cfg->cpu_type);
        qdev_prop_set_bit   (cpudev, "enable-bitband", false); /* M33: none */
        /* Reset reads the vector table (initial SP + reset PC) from flash. */
        qdev_prop_set_uint32(cpudev, "init-svtor",    cfg->flash_base);
        if (i > 0) {
            /* Secondary core(s) wait for an explicit SYSCON release. */
            qdev_prop_set_bit(cpudev, "start-powered-off", true);
        }
        qdev_connect_clock_in(cpudev, "cpuclk", s->sysclk);
        qdev_connect_clock_in(cpudev, "refclk", s->refclk);
        object_property_set_link(OBJECT(&s->armv7m[i]), "memory",
                                 OBJECT(&s->cpu_mem[i]), &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m[i]), errp)) {
            return;
        }
    }

    /* --- Peripherals ----------------------------------------------------- *
     * Catch-all over BOTH peripheral aliases.  MCX N is TrustZone-M: every
     * peripheral is mapped twice — non-secure at 0x400x_xxxx and secure at
     * 0x500x_xxxx (CMSIS confirms bases up to ~0x4012_3000 / 0x5012_3000).
     * One region spanning 0x4000_0000..0x5FFF_FFFF covers both.  Run with
     *   -d unimp,guest_errors
     * to see every access, then replace slices with real models.
     *
     * Verified bases for the first real models (MCXN947 CMSIS, NS alias):
     *   - FlexComm4 / LPUART4  @ 0x400B_4000  -> FRDM debug console
     *       (same LPUART register block as i.MX 93/95 — reuse that model;
     *        the LP_FLEXCOMM wrapper just function-selects USART vs SPI/I2C)
     *   - SCG0 (clock gen)     @ 0x4004_4000
     *   - PORT0..5 / GPIO0..5  @ 0x4011_6000.. / 0x4009_6000..
     *   - CAN0 / CAN1 (FlexCAN)@ 0x400D_4000 / 0x400D_8000
     *   - eIQ Neutron NPU: IRQ 97; base from RM (not in CMSIS header).
     *       Model behaviourally (cf. the i.MX 95 Neutron / ZV3400 approach).
     */
    create_unimplemented_device("mcxn.periph", 0x40000000, 0x20000000);

    /* FlexComm4 / LPUART4 console.  Mapped at default priority, so it overrides
     * the low-priority catch-all at this address.  serial_hd(0) wires it to the
     * board's -serial chardev (e.g. -serial mon:stdio). */
    qdev_prop_set_chr(DEVICE(&s->flexcomm4), "chardev", serial_hd(0));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexcomm4), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexcomm4), 0, MCXN_FLEXCOMM4_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexcomm4), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), MCXN_FLEXCOMM4_IRQ));
    /* Secure alias so TrustZone-secure firmware reaches the same console regs. */
    memory_region_init_alias(&s->flexcomm4_s_alias, OBJECT(dev),
                             "mcxn.flexcomm4.s", &s->flexcomm4.iomem, 0, 0x1000);
    memory_region_add_subregion(system_memory,
                                MCXN_FLEXCOMM4_BASE + MCXN_SECURE_ALIAS,
                                &s->flexcomm4_s_alias);

    /* SCG0 clock generator (stub: reports oscillators/PLLs ready). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->scg0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->scg0), 0, MCXN_SCG0_BASE);
    memory_region_init_alias(&s->scg0_s_alias, OBJECT(dev),
                             "mcxn.scg0.s", &s->scg0.iomem, 0, MCXN_SCG_SIZE);
    memory_region_add_subregion(system_memory,
                                MCXN_SCG0_BASE + MCXN_SECURE_ALIAS,
                                &s->scg0_s_alias);

    /* SYSCON: models the CPUCTRL/CPBOOT handover cpu0 uses to release cpu1.
     * Linked to the secondary core so a CPUCTRL write can start it. */
    if (ncpu > 1) {
        object_property_set_link(OBJECT(&s->syscon), "cpu1",
                                 OBJECT(s->armv7m[1].cpu), &error_abort);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->syscon), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->syscon), 0, MCXN_SYSCON_BASE);
    memory_region_init_alias(&s->syscon_s_alias, OBJECT(dev),
                             "mcxn.syscon.s", &s->syscon.iomem, 0,
                             MCXN_SYSCON_SIZE);
    memory_region_add_subregion(system_memory,
                                MCXN_SYSCON_BASE + MCXN_SECURE_ALIAS,
                                &s->syscon_s_alias);

    /* SPC system power controller (SRAMCTL REQ/ACK handshake for boot). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->spc0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->spc0), 0, MCXN_SPC0_BASE);
    memory_region_init_alias(&s->spc0_s_alias, OBJECT(dev), "mcxn.spc0.s",
                             &s->spc0.iomem, 0, MCXN_SPC_SIZE);
    memory_region_add_subregion(system_memory, MCXN_SPC0_BASE + MCXN_SECURE_ALIAS,
                                &s->spc0_s_alias);

    /* GPIO0..5 controllers and PORT0..5 pin-mux, each NS + secure alias. */
    for (i = 0; i < MCXN_NUM_GPIO; i++) {
        g_autofree char *aname = g_strdup_printf("mcxn.gpio%d.s", i);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio[i]), 0, mcxn_gpio_base[i]);
        memory_region_init_alias(&s->gpio_s_alias[i], OBJECT(dev), aname,
                                 &s->gpio[i].iomem, 0, MCXN_GPIO_SIZE);
        memory_region_add_subregion(system_memory,
                                     mcxn_gpio_base[i] + MCXN_SECURE_ALIAS,
                                     &s->gpio_s_alias[i]);
    }
    for (i = 0; i < MCXN_NUM_PORT; i++) {
        g_autofree char *aname = g_strdup_printf("mcxn.port%d.s", i);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->port[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->port[i]), 0, mcxn_port_base[i]);
        memory_region_init_alias(&s->port_s_alias[i], OBJECT(dev), aname,
                                 &s->port[i].iomem, 0, MCXN_PORT_SIZE);
        memory_region_add_subregion(system_memory,
                                     mcxn_port_base[i] + MCXN_SECURE_ALIAS,
                                     &s->port_s_alias[i]);
    }
}

static const Property mcxn_soc_properties[] = {
    DEFINE_PROP_STRING("part", MCXNState, part),
};

static void mcxn_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_soc_realize;
    device_class_set_props(dc, mcxn_soc_properties);
    /* SoC container: not directly user-creatable. */
    dc->user_creatable = false;
}

static const TypeInfo mcxn_soc_types[] = {
    {
        .name          = TYPE_MCXN_SOC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNState),
        .instance_init = mcxn_soc_instance_init,
        .class_init    = mcxn_soc_class_init,
    },
};

DEFINE_TYPES(mcxn_soc_types)
