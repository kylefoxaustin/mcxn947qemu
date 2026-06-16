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
#include "hw/misc/mcxn_stub.h"
#include "hw/misc/mcxn_crc.h"
#include "hw/misc/mcxn_cdog.h"
#include "hw/misc/mcxn_ewm.h"
#include "hw/misc/mcxn_inputmux.h"
#include "hw/misc/mcxn_evtg.h"
#include "hw/misc/mcxn_plu.h"
#include "hw/misc/mcxn_freqme.h"
#include "hw/misc/mcxn_gdet.h"
#include "hw/misc/mcxn_itrc.h"
#include "hw/misc/mcxn_tdet.h"
#include "hw/misc/mcxn_cmc.h"
#include "hw/misc/mcxn_eim.h"
#include "hw/misc/mcxn_erm.h"
#include "hw/misc/mcxn_intm.h"
#include "hw/misc/mcxn_cmx_perfmon.h"
#include "hw/misc/mcxn_sema42.h"
#include "hw/misc/mcxn_mailbox.h"
#include "hw/misc/mcxn_vbat.h"
#include "hw/misc/mcxn_wuu.h"
#include "hw/misc/mcxn_otpc.h"
#include "hw/misc/mcxn_cache64_ctrl.h"
#include "hw/misc/mcxn_ahbsc.h"
#include "hw/misc/mcxn_bsp32.h"
#include "hw/misc/mcxn_dm.h"
#include "hw/misc/mcxn_pint.h"
#include "hw/misc/mcxn_utick.h"
#include "hw/misc/mcxn_wwdt.h"
#include "hw/misc/mcxn_opamp.h"
#include "hw/misc/mcxn_cmp.h"
#include "hw/misc/mcxn_vref.h"
#include "system/address-spaces.h"
#include "system/system.h"             /* serial_hd (older trees: sysemu/sysemu.h) */
#include "target/arm/cpu-qom.h" /* ARM_CPU_TYPE_NAME */

/* FRDM-MCXN947 debug console: FlexComm4 / LPUART4 (NS alias) + its NVIC line. */
#define MCXN_FLEXCOMM4_BASE  0x400B4000
#define MCXN_FLEXCOMM4_IRQ   39          /* CMSIS: LP_FLEXCOMM4_IRQn */
#define MCXN_SCG0_BASE       0x40044000  /* system clock generator (NS alias) */
#define MCXN_SYSCON_BASE     0x40000000  /* SYSCON (NS alias); CPU1 boot ctrl */
#define MCXN_SPC0_BASE       0x40045000  /* system power controller (NS alias) */

/* On-chip memory apertures (RM Table 16): each memory has a non-secure base
 * and a secure-alias base (TZ-M). */
#define MCXN_FLASH_NS   0x00000000        /* program flash, 2 MB             */
#define MCXN_FLASH_S    0x10000000
#define MCXN_ROM_NS     0x03000000        /* boot ROM, 256 KB               */
#define MCXN_ROM_S      0x13000000
#define MCXN_ROM_SIZE   (256 * KiB)
#define MCXN_SRAMX_NS   0x04000000        /* SRAMX (RAMX), 96 KB            */
#define MCXN_SRAMX_S    0x14000000
#define MCXN_SRAMX_SIZE (96 * KiB)
#define MCXN_SRAM_NS    0x20000000        /* main SRAM RAMA..H, 416 KB     */
#define MCXN_SRAM_S     0x30000000

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
        .flash_base    = 0x10000000,   /* secure flash aperture (boot/svtor)  */
        .flash_size    = 2 * MiB,
        .sram_base     = 0x20000000,   /* main SRAM (RAMA..H) system-bus view */
        .sram_size     = 416 * KiB,    /* RAMA..H total (RM Table 16)         */
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

/* CTIMER0..4: NS base + NVIC IRQ (CMSIS). */
static const struct { hwaddr base; int irq; } mcxn_ctimer_cfg[MCXN_NUM_CTIMER] = {
    { 0x4000C000, 31 }, { 0x4000D000, 32 }, { 0x4000E000, 34 },
    { 0x4000F000, 55 }, { 0x40010000, 56 },
};

#define MCXN_MRT0_BASE  0x40013000   /* Multi-Rate Timer */
#define MCXN_MRT0_IRQ   30

/* LPTMR0..1: NS base + NVIC IRQ (CMSIS). */
static const struct { hwaddr base; int irq; } mcxn_lptmr_cfg[MCXN_NUM_LPTMR] = {
    { 0x4004A000, 143 }, { 0x4004B000, 144 },
};

/* LP_FLEXCOMM0..9 in LPUART mode: base, NVIC IRQ, and host -serial index
 * (-1 = no backend).  FlexComm4 = cpu0 console, FlexComm2 = cpu1 console. */
static const struct { hwaddr base; int irq; int serial; }
mcxn_flexcomm_cfg[MCXN_NUM_FLEXCOMM] = {
    { 0x40092000, 35, -1 }, { 0x40093000, 36, -1 }, { 0x40094000, 37,  1 },
    { 0x40095000, 38, -1 }, { 0x400B4000, 39,  0 }, { 0x400B5000, 40, -1 },
    { 0x400B6000, 41, -1 }, { 0x400B7000, 42, -1 }, { 0x400B8000, 43, -1 },
    { 0x400B9000, 44, -1 },
};

/* Every other peripheral present on the SoC, covered by the generic permissive
 * stub until it gets a real model (see mcxn_peripherals.inc). */
typedef struct MCXNStubDesc {
    hwaddr      base;
    uint64_t    size;
    const char *name;
} MCXNStubDesc;

static const MCXNStubDesc mcxn_stub_table[] = {
#include "mcxn_peripherals.inc"
};

/* Functional register-accurate config blocks (MMIO only, no IRQ/clock). */
static const struct { const char *type; hwaddr base; } mcxn_cfgdev[] = {
    { TYPE_MCXN_CRC,      0x400CB000 },
    { TYPE_MCXN_CDOG,     0x400BB000 },   /* CDOG0 */
    { TYPE_MCXN_CDOG,     0x400BC000 },   /* CDOG1 */
    { TYPE_MCXN_EWM,      0x400C0000 },
    { TYPE_MCXN_INPUTMUX, 0x40006000 },
    { TYPE_MCXN_EVTG,     0x400D2000 },
    { TYPE_MCXN_PLU,      0x40034000 },
    { TYPE_MCXN_FREQME,   0x40011000 },
    { TYPE_MCXN_GDET,     0x40024000 },   /* GDET0 */
    { TYPE_MCXN_GDET,     0x40025000 },   /* GDET1 */
    { TYPE_MCXN_ITRC,     0x40026000 },
    { TYPE_MCXN_TDET,     0x40058000 },
    { TYPE_MCXN_CMC,      0x40048000 },
    { TYPE_MCXN_EIM,      0x4005B000 },
    { TYPE_MCXN_ERM,      0x4005C000 },
    { TYPE_MCXN_INTM,     0x4005D000 },
    { TYPE_MCXN_CMX_PERFMON, 0x400C1000 },   /* CMX_PERFMON0 */
    { TYPE_MCXN_CMX_PERFMON, 0x400C2000 },   /* CMX_PERFMON1 */
    { TYPE_MCXN_SEMA42,   0x400B1000 },
    { TYPE_MCXN_MAILBOX,  0x400B2000 },
    { TYPE_MCXN_VBAT,     0x40059000 },
    { TYPE_MCXN_WUU,      0x40046000 },
    { TYPE_MCXN_OTPC,     0x400C9000 },
    /* One CACHE64 device covers the full window (POLSEL @0x14, CTRL @0x800). */
    { TYPE_MCXN_CACHE64_CTRL, 0x4001B000 },
    { TYPE_MCXN_AHBSC,    0x40120000 },
    { TYPE_MCXN_BSP32,    0x40032000 },
    { TYPE_MCXN_DM,       0x400BD000 },
    { TYPE_MCXN_PINT,     0x40004000 },
    { TYPE_MCXN_UTICK,    0x40012000 },
    { TYPE_MCXN_WWDT,     0x40016000 },   /* WWDT0 */
    { TYPE_MCXN_WWDT,     0x40017000 },   /* WWDT1 */
    { TYPE_MCXN_OPAMP,    0x40110000 },   /* OPAMP0 */
    { TYPE_MCXN_OPAMP,    0x40113000 },   /* OPAMP1 */
    { TYPE_MCXN_OPAMP,    0x40115000 },   /* OPAMP2 */
    { TYPE_MCXN_CMP,      0x40051000 },   /* CMP0 */
    { TYPE_MCXN_CMP,      0x40052000 },   /* CMP1 */
    { TYPE_MCXN_CMP,      0x40053000 },   /* CMP2 */
    { TYPE_MCXN_VREF,     0x40111000 },
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
    for (i = 0; i < MCXN_NUM_FLEXCOMM; i++) {
        g_autofree char *name = g_strdup_printf("flexcomm%d", i);
        object_initialize_child(obj, name, &s->flexcomm[i], TYPE_MCXN_LPUART);
    }
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
    for (i = 0; i < MCXN_NUM_CTIMER; i++) {
        g_autofree char *name = g_strdup_printf("ctimer%d", i);
        object_initialize_child(obj, name, &s->ctimer[i], TYPE_MCXN_CTIMER);
    }
    object_initialize_child(obj, "mrt0", &s->mrt0, TYPE_MCXN_MRT);
    for (i = 0; i < MCXN_NUM_LPTMR; i++) {
        g_autofree char *name = g_strdup_printf("lptmr%d", i);
        object_initialize_child(obj, name, &s->lptmr[i], TYPE_MCXN_LPTMR);
    }
    object_initialize_child(obj, "fmu0", &s->fmu0, TYPE_MCXN_FMU);
    object_initialize_child(obj, "ostimer0", &s->ostimer0, TYPE_MCXN_OSTIMER);

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

    /* --- On-chip memories (RM Table 16) ---------------------------------- *
     * Flash, boot ROM, SRAM and SRAMX, each reachable via a non-secure base
     * and a secure-alias base (TZ-M).  Flash and ROM are RAM-backed during
     * bring-up (the -kernel loader and, later, the FMU write them); swap flash
     * to ROM + FMU program/erase once that path is exercised.
     */
    memory_region_init_ram(&s->flash, OBJECT(dev), "mcxn.flash",
                           cfg->flash_size, &error_fatal);
    memory_region_add_subregion(system_memory, MCXN_FLASH_NS, &s->flash);
    memory_region_init_alias(&s->flash_alias, OBJECT(dev), "mcxn.flash.s",
                             &s->flash, 0, cfg->flash_size);
    memory_region_add_subregion(system_memory, MCXN_FLASH_S, &s->flash_alias);

    memory_region_init_ram(&s->rom, OBJECT(dev), "mcxn.rom-boot",
                           MCXN_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, MCXN_ROM_NS, &s->rom);
    memory_region_init_alias(&s->rom_alias, OBJECT(dev), "mcxn.rom-boot.s",
                             &s->rom, 0, MCXN_ROM_SIZE);
    memory_region_add_subregion(system_memory, MCXN_ROM_S, &s->rom_alias);

    memory_region_init_ram(&s->sramx, OBJECT(dev), "mcxn.sramx",
                           MCXN_SRAMX_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, MCXN_SRAMX_NS, &s->sramx);
    memory_region_init_alias(&s->sramx_alias, OBJECT(dev), "mcxn.sramx.s",
                             &s->sramx, 0, MCXN_SRAMX_SIZE);
    memory_region_add_subregion(system_memory, MCXN_SRAMX_S, &s->sramx_alias);

    memory_region_init_ram(&s->sram, OBJECT(dev), "mcxn.sram",
                           cfg->sram_size, &error_fatal);
    memory_region_add_subregion(system_memory, MCXN_SRAM_NS, &s->sram);
    memory_region_init_alias(&s->sram_alias, OBJECT(dev), "mcxn.sram.s",
                             &s->sram, 0, cfg->sram_size);
    memory_region_add_subregion(system_memory, MCXN_SRAM_S, &s->sram_alias);

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

    /* LP_FLEXCOMM0..9 as LPUARTs: each NS-mapped + secure alias, NVIC line
     * connected.  The console instances bind a host -serial chardev
     * (FlexComm4 = cpu0 console on serial_hd(0); FlexComm2 = cpu1 on
     * serial_hd(1)); the rest run without a host backend. */
    for (i = 0; i < MCXN_NUM_FLEXCOMM; i++) {
        DeviceState *fc = DEVICE(&s->flexcomm[i]);
        g_autofree char *aname = g_strdup_printf("mcxn.flexcomm%d.s", i);
        Chardev *chr = (mcxn_flexcomm_cfg[i].serial >= 0)
                       ? serial_hd(mcxn_flexcomm_cfg[i].serial) : NULL;

        if (chr) {
            qdev_prop_set_chr(fc, "chardev", chr);
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexcomm[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexcomm[i]), 0,
                        mcxn_flexcomm_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexcomm[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            mcxn_flexcomm_cfg[i].irq));
        memory_region_init_alias(&s->flexcomm_s_alias[i], OBJECT(dev), aname,
                                 &s->flexcomm[i].iomem, 0, 0x1000);
        memory_region_add_subregion(system_memory,
                                     mcxn_flexcomm_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->flexcomm_s_alias[i]);
    }

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

    /* CTIMER0..4: functional counter/timers, IRQ to cpu0 NVIC, clocked by the
     * SoC main clock (real divider lives in the stubbed clock tree). */
    for (i = 0; i < MCXN_NUM_CTIMER; i++) {
        DeviceState *t = DEVICE(&s->ctimer[i]);
        g_autofree char *aname = g_strdup_printf("mcxn.ctimer%d.s", i);

        qdev_connect_clock_in(t, "clk", s->sysclk);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->ctimer[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->ctimer[i]), 0, mcxn_ctimer_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->ctimer[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            mcxn_ctimer_cfg[i].irq));
        memory_region_init_alias(&s->ctimer_s_alias[i], OBJECT(dev), aname,
                                 &s->ctimer[i].iomem, 0, 0x1000);
        memory_region_add_subregion(system_memory,
                                     mcxn_ctimer_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->ctimer_s_alias[i]);
    }

    /* MRT (Multi-Rate Timer): functional, IRQ to cpu0 NVIC. */
    qdev_connect_clock_in(DEVICE(&s->mrt0), "clk", s->sysclk);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mrt0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mrt0), 0, MCXN_MRT0_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mrt0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), MCXN_MRT0_IRQ));
    memory_region_init_alias(&s->mrt0_s_alias, OBJECT(dev), "mcxn.mrt0.s",
                             &s->mrt0.iomem, 0, 0x1000);
    memory_region_add_subregion(system_memory, MCXN_MRT0_BASE + MCXN_SECURE_ALIAS,
                                &s->mrt0_s_alias);

    /* LPTMR0..1: functional, IRQ to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_LPTMR; i++) {
        DeviceState *t = DEVICE(&s->lptmr[i]);
        g_autofree char *aname = g_strdup_printf("mcxn.lptmr%d.s", i);

        qdev_connect_clock_in(t, "clk", s->sysclk);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->lptmr[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->lptmr[i]), 0, mcxn_lptmr_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->lptmr[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            mcxn_lptmr_cfg[i].irq));
        memory_region_init_alias(&s->lptmr_s_alias[i], OBJECT(dev), aname,
                                 &s->lptmr[i].iomem, 0, 0x1000);
        memory_region_add_subregion(system_memory,
                                     mcxn_lptmr_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->lptmr_s_alias[i]);
    }

    /* OSTIMER (OS event timer): 1 MHz default clock, match IRQ to cpu0 NVIC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ostimer0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ostimer0), 0, 0x40049000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->ostimer0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 57));
    memory_region_init_alias(&s->ostimer0_s_alias, OBJECT(dev), "mcxn.ostimer0.s",
                             &s->ostimer0.iomem, 0, 0x1000);
    memory_region_add_subregion(system_memory, 0x40049000 + MCXN_SECURE_ALIAS,
                                &s->ostimer0_s_alias);

    /* FMU flash controller: knows the flash backing so erase/verify work;
     * IRQ to cpu0 NVIC. */
    s->fmu0.flash = &s->flash;
    s->fmu0.flash_size = cfg->flash_size;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->fmu0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->fmu0), 0, 0x40043000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->fmu0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 138));
    memory_region_init_alias(&s->fmu0_s_alias, OBJECT(dev), "mcxn.fmu0.s",
                             &s->fmu0.iomem, 0, 0x1000);
    memory_region_add_subregion(system_memory, 0x40043000 + MCXN_SECURE_ALIAS,
                                &s->fmu0_s_alias);

    /* Functional register-accurate config blocks (MMIO only): each NS + secure
     * alias.  Instantiated dynamically since they need no IRQ/clock wiring. */
    for (i = 0; i < (int)ARRAY_SIZE(mcxn_cfgdev); i++) {
        DeviceState *d = qdev_new(mcxn_cfgdev[i].type);
        MemoryRegion *al = g_new(MemoryRegion, 1);
        g_autofree char *cn = g_strdup_printf("cfgdev%d", i);
        g_autofree char *an = g_strdup_printf("mcxn.cfg%d.s", i);
        MemoryRegion *mr;

        object_property_add_child(OBJECT(dev), cn, OBJECT(d));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(d), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(d), 0, mcxn_cfgdev[i].base);
        mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(d), 0);
        memory_region_init_alias(al, OBJECT(dev), an, mr, 0,
                                 memory_region_size(mr));
        memory_region_add_subregion(system_memory,
                                     mcxn_cfgdev[i].base + MCXN_SECURE_ALIAS, al);
    }

    /* Generic permissive stubs for every other peripheral present on the SoC
     * (present + register read-back + non-blocking), each NS + secure alias.
     * Replace entries with real device models over time. */
    for (i = 0; i < (int)ARRAY_SIZE(mcxn_stub_table); i++) {
        const MCXNStubDesc *d = &mcxn_stub_table[i];
        DeviceState *stub = qdev_new(TYPE_MCXN_STUB);
        MemoryRegion *salias = g_new(MemoryRegion, 1);
        g_autofree char *sname = g_strdup_printf("mcxn.%s.s", d->name);

        object_property_add_child(OBJECT(dev), d->name, OBJECT(stub));
        qdev_prop_set_string(stub, "blkname", d->name);
        qdev_prop_set_uint64(stub, "size", d->size);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(stub), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(stub), 0, d->base);
        memory_region_init_alias(salias, OBJECT(dev), sname,
                                 sysbus_mmio_get_region(SYS_BUS_DEVICE(stub), 0),
                                 0, d->size);
        memory_region_add_subregion(system_memory,
                                     d->base + MCXN_SECURE_ALIAS, salias);
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
