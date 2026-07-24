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
#include "chardev/char.h"                   /* qemu_chr_find */
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
#include "hw/misc/mcxn_els.h"
#include "hw/misc/mcxn_puf.h"
#include "hw/misc/mcxn_pkc.h"
#include "hw/misc/mcxn_trdc.h"
#include "hw/misc/mcxn_dac.h"
#include "hw/misc/mcxn_sinc.h"
#include "hw/misc/mcxn_pdm.h"
#include "hw/misc/mcxn_emvsim.h"
#include "hw/misc/mcxn_i3c.h"
#include "hw/misc/mcxn_flexio.h"
#include "hw/misc/mcxn_flexcan.h"
#include "hw/misc/mcxn_enet.h"
#include "hw/misc/mcxn_adc.h"
#include "hw/misc/mcxn_rtc.h"
#include "hw/misc/mcxn_tsi.h"
#include "hw/misc/mcxn_sai.h"
#include "hw/misc/mcxn_usdhc.h"
#include "hw/misc/mcxn_flexspi.h"
#include "hw/ssi/ssi.h"
#include "hw/misc/mcxn_pwm.h"
#include "hw/misc/mcxn_qdc.h"
#include "hw/misc/mcxn_sct.h"
#include "hw/misc/mcxn_usbfs.h"
#include "hw/misc/mcxn_usbdcd.h"
#include "hw/misc/mcxn_usbphy.h"
#include "hw/misc/mcxn_usbhs.h"
#include "hw/misc/mcxn_smartdma.h"
#include "hw/misc/mcxn_powerquad.h"
#include "hw/misc/mcxn_npu.h"
#include "hw/misc/mcxn_neutron.h"
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
#define MCXN_FLEXSPI0_AHB_NS 0x80000000   /* FlexSPI0 AHB-mapped NOR (XIP)  */
#define MCXN_FLEXSPI0_AHB_S  0x90000000   /* secure alias                   */

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
    /* MAILBOX 0x400B2000 instantiated explicitly below (cross-core IRQ wired). */
    { TYPE_MCXN_VBAT,     0x40059000 },
    { TYPE_MCXN_WUU,      0x40046000 },
    { TYPE_MCXN_OTPC,     0x400C9000 },
    /* One CACHE64 device covers the full window (POLSEL @0x14, CTRL @0x800). */
    { TYPE_MCXN_CACHE64_CTRL, 0x4001B000 },
    { TYPE_MCXN_AHBSC,    0x40120000 },
    { TYPE_MCXN_BSP32,    0x40032000 },
    { TYPE_MCXN_DM,       0x400BD000 },
    /* PINT 0x40004000 instantiated explicitly below (operator-driven pin input +
     * NVIC IRQ 47 + INT0..3 eDMA request lines). */
    { TYPE_MCXN_UTICK,    0x40012000 },
    { TYPE_MCXN_WWDT,     0x40016000 },   /* WWDT0 */
    { TYPE_MCXN_WWDT,     0x40017000 },   /* WWDT1 */
    { TYPE_MCXN_OPAMP,    0x40110000 },   /* OPAMP0 */
    { TYPE_MCXN_OPAMP,    0x40113000 },   /* OPAMP1 */
    { TYPE_MCXN_OPAMP,    0x40115000 },   /* OPAMP2 */
    /* CMP0..2 instantiated explicitly below (operator-driven output + IRQ). */
    { TYPE_MCXN_VREF,     0x40111000 },
    /* Security: ELS (EdgeLock), PUF, PKC (public-key crypto), TRDC. */
    { TYPE_MCXN_ELS,      0x40054000 },
    { TYPE_MCXN_PUF,      0x4002C000 },
    { TYPE_MCXN_PKC,      0x4002B000 },
    { TYPE_MCXN_TRDC,     0x400C7000 },
    /* (Analog/audio: SINC + PDM + DAC0..2 + EMVSIM0/1 below — IRQs wired.) */
    /* Comm/serial: FlexIO.  (I3C0/1 instantiated below — IRQs wired.) */
    { TYPE_MCXN_FLEXIO,   0x40105000 },
    /* Connectivity: FlexCAN0/1 and ENET instantiated below (IRQs wired). */
    /* Analog: TSI0 + ADC0/1 instantiated explicitly below (operator-driven
     * inputs + IRQ).  RTC also below. */
    /* (SAI0/1 + uSDHC + FlexSPI instantiated below — IRQs wired.) */
    /* Motor/timer: QDC0/1.  (eFlexPWM0/1 + SCT below — IRQs wired.) */
    { TYPE_MCXN_QDC,      0x400CF000 },   /* QDC0 */
    { TYPE_MCXN_QDC,      0x400D1000 },   /* QDC1 */
    /* USB: FS-OTG, charger detect, HS PHY + HS core/non-core (OBMF-ICP path). */
    /* USBFS0 @ 0x400DD000 instantiated explicitly (device-mode engine + IRQ). */
    { TYPE_MCXN_USBDCD,       0x400DC000 },
    { TYPE_MCXN_USBPHY,       0x4010A000 },   /* 0x800 window */
    { TYPE_MCXN_USBHS_PHYDCD, 0x4010A800 },   /* 0x800 window */
    /* USBHS core @ 0x4010B000 instantiated explicitly (device engine + IRQ). */
    { TYPE_MCXN_USBHS_NC,     0x4010B200 },   /* 0xE00 window */
    /* Accelerators: SmartDMA, eIQ Neutron NPU (NPX).  (PowerQuad below — IRQ.) */
    { TYPE_MCXN_SMARTDMA,  0x40033000 },
    { TYPE_MCXN_NPU,       0x400CC000 },
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
        /* The RM's pad reset values DIFFER PER PORT (PORT0's SWD pins are non-zero),
         * so each PORT must know which one it is. */
        qdev_prop_set_uint8(DEVICE(&s->port[i]), "port-id", i);
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
    object_initialize_child(obj, "sinc0", &s->sinc0, TYPE_MCXN_SINC);
    object_initialize_child(obj, "pdm0", &s->pdm0, TYPE_MCXN_PDM);
    object_initialize_child(obj, "ostimer0", &s->ostimer0, TYPE_MCXN_OSTIMER);
    object_initialize_child(obj, "inputmux0", &s->inputmux, TYPE_MCXN_INPUTMUX);
    for (i = 0; i < MCXN_NUM_EDMA; i++) {
        g_autofree char *name = g_strdup_printf("edma%d", i);
        object_initialize_child(obj, name, &s->edma[i], TYPE_MCXN_EDMA);
        /* CH_SBR[MID] -- the BUS MASTER ID -- differs per instance (RM: DMA0=6, DMA1=7),
         * and Linux read-modify-writes that register, so a wrong reset gets laundered
         * into the guest's own configuration. */
        qdev_prop_set_uint8(DEVICE(&s->edma[i]), "dma-id", i);
    }
    for (i = 0; i < MCXN_NUM_ADC; i++) {
        g_autofree char *name = g_strdup_printf("adc%d", i);
        object_initialize_child(obj, name, &s->adc[i], TYPE_MCXN_ADC);
    }
    for (i = 0; i < MCXN_NUM_CMP; i++) {
        g_autofree char *name = g_strdup_printf("cmp%d", i);
        object_initialize_child(obj, name, &s->cmp[i], TYPE_MCXN_CMP);
    }
    object_initialize_child(obj, "pint0", &s->pint0, TYPE_MCXN_PINT);
    for (i = 0; i < MCXN_NUM_TSI; i++) {
        g_autofree char *name = g_strdup_printf("tsi%d", i);
        object_initialize_child(obj, name, &s->tsi[i], TYPE_MCXN_TSI);
    }
    for (i = 0; i < MCXN_NUM_EMVSIM; i++) {
        g_autofree char *name = g_strdup_printf("emvsim%d", i);
        object_initialize_child(obj, name, &s->emvsim[i], TYPE_MCXN_EMVSIM);
    }
    for (i = 0; i < MCXN_NUM_FLEXCAN; i++) {
        g_autofree char *name = g_strdup_printf("flexcan%d", i);
        object_initialize_child(obj, name, &s->flexcan[i], TYPE_MCXN_FLEXCAN);
    }
    object_initialize_child(obj, "enet0", &s->enet0, TYPE_MCXN_ENET);
    object_initialize_child(obj, "mailbox", &s->mailbox, TYPE_MCXN_MAILBOX);
    object_initialize_child(obj, "rtc0", &s->rtc0, TYPE_MCXN_RTC);
    object_initialize_child(obj, "usdhc0", &s->usdhc0, TYPE_MCXN_USDHC);
    object_initialize_child(obj, "flexspi0", &s->flexspi0, TYPE_MCXN_FLEXSPI);
    object_initialize_child(obj, "usbdev", &s->usbdev, TYPE_MCXN_USBDEV);
    object_initialize_child(obj, "usbfs0", &s->usbfs0, TYPE_MCXN_USBFS);
    object_initialize_child(obj, "usbdev-hs", &s->usbdev_hs, TYPE_MCXN_USBDEV);
    object_initialize_child(obj, "usbhs-core", &s->usbhs_core,
                            TYPE_MCXN_USBHS_CORE);
    object_initialize_child(obj, "neutron0", &s->neutron0, TYPE_MCXN_NEUTRON);
    for (i = 0; i < MCXN_NUM_SAI; i++) {
        g_autofree char *name = g_strdup_printf("sai%d", i);
        object_initialize_child(obj, name, &s->sai[i], TYPE_MCXN_SAI);
    }
    for (i = 0; i < MCXN_NUM_DAC; i++) {
        g_autofree char *name = g_strdup_printf("dac%d", i);
        object_initialize_child(obj, name, &s->dac[i], TYPE_MCXN_DAC);
    }
    object_initialize_child(obj, "powerquad0", &s->powerquad0,
                            TYPE_MCXN_POWERQUAD);
    for (i = 0; i < MCXN_NUM_PWM; i++) {
        g_autofree char *name = g_strdup_printf("pwm%d", i);
        object_initialize_child(obj, name, &s->pwm[i], TYPE_MCXN_PWM);
    }
    object_initialize_child(obj, "sct0", &s->sct0, TYPE_MCXN_SCT);
    for (i = 0; i < MCXN_NUM_I3C; i++) {
        g_autofree char *name = g_strdup_printf("i3c%d", i);
        object_initialize_child(obj, name, &s->i3c[i], TYPE_MCXN_I3C);
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

    /* --- On-chip memories (RM Table 16) ---------------------------------- *
     * Flash, boot ROM, SRAM and SRAMX, each reachable via a non-secure base
     * and a secure-alias base (TZ-M).
     *
     * Flash is a ROM *device* owned by the FMU, not RAM: reads and instruction
     * fetch are direct (so XIP and the -kernel ROM loader are unaffected), but
     * guest stores are routed into the FMU, which only honours them inside an
     * open PEWEN program/erase window.  Making it plain RAM would let firmware
     * scribble at flash addresses and appear to work, which silicon would not
     * do — see hw/misc/mcxn_fmu.c.
     */
    mcxn_fmu_init_flash(&s->fmu0, OBJECT(dev), &s->flash, cfg->flash_size,
                        &error_fatal);
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
    /* SCG0 clock generator — realized here, BEFORE the cores, so its derived main
     * clock (RCCR[SCS] -> FRO_HF at reset -> 48 MHz; PLL0 after firmware -> 150 MHz)
     * can drive the M33 cpuclk/refclk below instead of a fixed board constant. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->scg0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->scg0), 0, MCXN_SCG0_BASE);
    memory_region_init_alias(&s->scg0_s_alias, OBJECT(dev),
                             "mcxn.scg0.s", &s->scg0.iomem, 0, MCXN_SCG_SIZE);
    memory_region_add_subregion(system_memory,
                                MCXN_SCG0_BASE + MCXN_SECURE_ALIAS,
                                &s->scg0_s_alias);

    /*
     * SYSCON is realized HERE, before the cores, so its AHB busclk output
     * (= SCG mainclk / (AHBCLKDIV+1)) can drive the M33 cpuclk/refclk below.  Its clock
     * inputs come from the SCG (already realized).  SYSCON's cpu1 link is set AFTER the cores
     * realize (a settable-anytime link, added in instance_init) -- SYSCON never reads cpu1 at
     * realize, only at runtime on the CPUCTRL write, so this breaks the busclk<->cpu1 cycle.
     * (qdev_connect_clock_in asserts !realized, so the inputs are connected first.)
     */
    qdev_connect_clock_in(DEVICE(&s->syscon), "fro12m",
                          qdev_get_clock_out(DEVICE(&s->scg0), "fro12m"));
    qdev_connect_clock_in(DEVICE(&s->syscon), "frohf",
                          qdev_get_clock_out(DEVICE(&s->scg0), "frohf"));
    qdev_connect_clock_in(DEVICE(&s->syscon), "apll",
                          qdev_get_clock_out(DEVICE(&s->scg0), "apll"));
    qdev_connect_clock_in(DEVICE(&s->syscon), "spll",
                          qdev_get_clock_out(DEVICE(&s->scg0), "spll"));
    qdev_connect_clock_in(DEVICE(&s->syscon), "mainclk",
                          qdev_get_clock_out(DEVICE(&s->scg0), "mainclk"));
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
        /* PowerQuad CP0 scalar-math coprocessor: both M33s have it on silicon. */
        qdev_prop_set_bit   (cpudev, "powerquad",     true);
        /* Reset reads the vector table (initial SP + reset PC) from flash.  In QSPI
         * execute-in-place boot the reset vector lives in the external FlexSPI NOR instead of
         * internal flash -- the production boot mode where there is no internal-flash image;
         * the boot ROM has (in this model) already configured FlexSPI.  Use the SECURE XIP
         * alias (0x9000_0000), consistent with the secure internal-flash boot (0x1000_0000)
         * and the addressable-as-memory NOR window firmware executes in place from. */
        qdev_prop_set_uint32(cpudev, "init-svtor",
                             s->qspi_boot ? MCXN_FLEXSPI0_AHB_S : cfg->flash_base);
        if (i > 0) {
            /* Secondary core(s) wait for an explicit SYSCON release. */
            qdev_prop_set_bit(cpudev, "start-powered-off", true);
        }
        /* The M33 core + SysTick derive from the AHB busclk = SCG mainclk / (AHBCLKDIV+1):
         * 48 MHz FRO_HF out of reset, 150 MHz once firmware brings up PLL0, and it follows an
         * AHBCLKDIV write.  refclk (the SysTick alternate reference) shares it -- the
         * SYSTICKCLKSEL divider is not modelled, so it tracks the core clock. */
        qdev_connect_clock_in(cpudev, "cpuclk",
                              qdev_get_clock_out(DEVICE(&s->syscon), "busclk"));
        qdev_connect_clock_in(cpudev, "refclk",
                              qdev_get_clock_out(DEVICE(&s->syscon), "busclk"));
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
        /* FlexComm5 is the LPSPI board-to-board node: expose a named SSI bus so
         * a `-device spi-link,bus=mcxn-lpspi,chardev=...` bridges it to a socket
         * (inter-QEMU SPI link, like the UART/USB b2b links). */
        if (i == 5) {
            qdev_prop_set_string(fc, "spi-bus-name", "mcxn-lpspi");
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

    /* SCG0 is realized BEFORE the cores (above) so its mainclk output can feed cpuclk. */

    /* SYSCON (realized above, before the cores) drives the CPUCTRL/CPBOOT handover cpu0 uses
     * to release cpu1.  cpu1 is set HERE -- after the cores realize, so armv7m[1].cpu exists;
     * SYSCON only reads it at runtime on the CPUCTRL write, never at realize, which is what
     * lets SYSCON realize BEFORE the cores (to feed cpuclk from busclk).  A direct field set:
     * the SoC owns SYSCON, the CPU lives for the machine's lifetime (no ref management), and a
     * DEFINE_PROP_LINK would have had to be set before realize -- the ordering we can't meet. */
    if (ncpu > 1) {
        s->syscon.cpu1 = s->armv7m[1].cpu;
    }

    /* Inter-CPU MAILBOX: cross-core notification.  IRQ[0]->cpu0, IRQ[1]->cpu1,
     * both on MAILBOX_IRQn = 54.  This is the rpmsg/OpenAMP signalling path. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mailbox), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mailbox), 0, 0x400B2000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mailbox), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 54));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->mailbox), 1,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[ncpu > 1 ? 1 : 0]), 54));
    memory_region_init_alias(&s->mailbox_s_alias, OBJECT(dev), "mcxn.mailbox.s",
                             &s->mailbox.iomem, 0, MCXN_MAILBOX_SIZE);
    memory_region_add_subregion(system_memory, 0x400B2000 + MCXN_SECURE_ALIAS,
                                &s->mailbox_s_alias);

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

        /*
         * The CTIMER's rate is DECIDED by SYSCON[CTIMERCLKSEL[i]] / CTIMERCLKDIV[i]
         * -- and this used to be hardwired to sysclk, so the selector did nothing.
         * Real firmware does CLOCK_AttachClk(kFRO_HF_to_CTIMER0), computes its match
         * values from CLOCK_GetCTimerClkFreq() = 48 MHz, and we ticked it at 150 MHz:
         * EVERY DELAY 3.1x TOO SHORT, silently.  Measured with SysTick before the fix:
         * 12011 ticks where the SDK's own arithmetic expects 150000.
         */
        g_autofree char *cn = g_strdup_printf("ctimer%d-clk", i);

        qdev_connect_clock_in(t, "clk",
                              qdev_get_clock_out(DEVICE(&s->syscon), cn));
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

    /* MRT (Multi-Rate Timer): runs on the AHB/bus clock, which is the SCG main clock
     * (kCLOCK_Mrt = AHB_CLK_CTRL1 gate, no selector).  Now DERIVED from mainclk -- 48 MHz
     * at reset, 150 MHz once firmware brings up PLL0 -- instead of the raw sysclk constant.
     * (AHBCLKDIV is not modelled; the core takes mainclk directly too, so both assume /1.) */
    qdev_connect_clock_in(DEVICE(&s->mrt0), "clk",
                          qdev_get_clock_out(DEVICE(&s->syscon), "busclk"));
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

    /* LPTMR0..1: functional, IRQ to cpu0 NVIC.  The "clk" input is FRO_12M (PSR[PCS]=00,
     * the reset default), driven from the SCG -- 12 MHz, a LOW-POWER clock, not the 150 MHz
     * bus clock the old model wrongly used (the LPTMR max is 25 MHz).  The other PCS sources
     * (FRO_16K, 32K_CLK, OSC_SYS) are resolved inside the model per RM Table 463. */
    for (i = 0; i < MCXN_NUM_LPTMR; i++) {
        DeviceState *t = DEVICE(&s->lptmr[i]);
        g_autofree char *aname = g_strdup_printf("mcxn.lptmr%d.s", i);

        qdev_connect_clock_in(t, "clk",
                              qdev_get_clock_out(DEVICE(&s->scg0), "fro12m"));
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

    /*
     * INPUTMUX0.  Realized BEFORE the eDMAs so its gate outputs exist, but WIRED
     * after them (see below): qdev_get_gpio_in_named() only resolves once the
     * TARGET is realized, and sysbus_connect_irq() only once the SOURCE is.  The
     * ordering is a constraint in BOTH directions, which is exactly how the
     * peripheral DMA request wiring got silently dropped once already.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->inputmux), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->inputmux), 0, 0x40006000);
    {
        MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->inputmux), 0);

        memory_region_init_alias(&s->inputmux_s_alias, OBJECT(dev),
                                 "mcxn.inputmux.s", mr, 0,
                                 memory_region_size(mr));
        memory_region_add_subregion(system_memory,
                                    0x40006000 + MCXN_SECURE_ALIAS,
                                    &s->inputmux_s_alias);
    }

    /* eDMA DMA0..1: 16 channels each, channel IRQs to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_EDMA; i++) {
        static const struct { hwaddr base; int irq0; }
        edma_cfg[MCXN_NUM_EDMA] = { { 0x40080000, 1 }, { 0x400A0000, 77 } };
        g_autofree char *aname = g_strdup_printf("mcxn.edma%d.s", i);
        uint64_t sz = 0x1000 * (MCXN_EDMA_CHANNELS + 1);
        int ch;

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->edma[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->edma[i]), 0, edma_cfg[i].base);
        for (ch = 0; ch < MCXN_EDMA_CHANNELS; ch++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->edma[i]), ch,
                               qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                                edma_cfg[i].irq0 + ch));
        }
        memory_region_init_alias(&s->edma_s_alias[i], OBJECT(dev), aname,
                                 &s->edma[i].iomem, 0, sz);
        memory_region_add_subregion(system_memory,
                                     edma_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->edma_s_alias[i]);
    }


    /* ADC0..1 (LPADC): conversion-complete IRQ to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_ADC; i++) {
        static const struct { hwaddr base; int irq; }
        adc_cfg[MCXN_NUM_ADC] = { { 0x4010D000, 45 }, { 0x4010E000, 46 } };
        g_autofree char *aname = g_strdup_printf("mcxn.adc%d.s", i);

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc[i]), 0, adc_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            adc_cfg[i].irq));
        memory_region_init_alias(&s->adc_s_alias[i], OBJECT(dev), aname,
                                 &s->adc[i].iomem, 0, MCXN_ADC_SIZE);
        memory_region_add_subregion(system_memory,
                                     adc_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->adc_s_alias[i]);
    }

    /* CMP0..2 (LPCMP): operator-driven output, edge-flag IRQ to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_CMP; i++) {
        static const struct { hwaddr base; int irq; }
        cmp_cfg[MCXN_NUM_CMP] = { { 0x40051000, 109 }, { 0x40052000, 110 },
                                  { 0x40053000, 111 } };
        g_autofree char *aname = g_strdup_printf("mcxn.cmp%d.s", i);

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->cmp[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->cmp[i]), 0, cmp_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->cmp[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            cmp_cfg[i].irq));
        memory_region_init_alias(&s->cmp_s_alias[i], OBJECT(dev), aname,
                                 &s->cmp[i].iomem, 0, MCXN_CMP_SIZE);
        memory_region_add_subregion(system_memory,
                                     cmp_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->cmp_s_alias[i]);
    }

    /* PINT (pin interrupt): operator-driven pin input, shared NVIC line PINT0_IRQn=47.
     * The INT0..3 eDMA request lines are connected in the DMA-request block below. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pint0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pint0), 0, 0x40004000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pint0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 47));
    memory_region_init_alias(&s->pint0_s_alias, OBJECT(dev), "mcxn.pint0.s",
                             &s->pint0.iomem, 0, MCXN_PINT_SIZE);
    memory_region_add_subregion(system_memory, 0x40004000 + MCXN_SECURE_ALIAS,
                                 &s->pint0_s_alias);

    /* TSI0 (touch sense): operator-driven per-channel count, end-of-scan IRQ. */
    for (i = 0; i < MCXN_NUM_TSI; i++) {
        static const struct { hwaddr base; int irq; }
        tsi_cfg[MCXN_NUM_TSI] = { { 0x40050000, 101 } };
        g_autofree char *aname = g_strdup_printf("mcxn.tsi%d.s", i);

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->tsi[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->tsi[i]), 0, tsi_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->tsi[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            tsi_cfg[i].irq));
        memory_region_init_alias(&s->tsi_s_alias[i], OBJECT(dev), aname,
                                 &s->tsi[i].iomem, 0, MCXN_TSI_SIZE);
        memory_region_add_subregion(system_memory,
                                     tsi_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->tsi_s_alias[i]);
    }

    /* EMVSIM0..1 (smartcard): transmit-complete IRQ to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_EMVSIM; i++) {
        static const struct { hwaddr base; int irq; }
        emvsim_cfg[MCXN_NUM_EMVSIM] = { { 0x40103000, 103 }, { 0x40104000, 104 } };
        g_autofree char *aname = g_strdup_printf("mcxn.emvsim%d.s", i);

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->emvsim[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->emvsim[i]), 0, emvsim_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->emvsim[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            emvsim_cfg[i].irq));
        memory_region_init_alias(&s->emvsim_s_alias[i], OBJECT(dev), aname,
                                 &s->emvsim[i].iomem, 0, MCXN_EMVSIM_SIZE);
        memory_region_add_subregion(system_memory,
                                     emvsim_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->emvsim_s_alias[i]);
    }

    /* FlexCAN CAN0..1: message-buffer interrupt to cpu0 NVIC.  0x4000 window. */
    for (i = 0; i < MCXN_NUM_FLEXCAN; i++) {
        static const struct { hwaddr base; int irq; }
        can_cfg[MCXN_NUM_FLEXCAN] = { { 0x400D4000, 62 }, { 0x400D8000, 63 } };
        g_autofree char *aname = g_strdup_printf("mcxn.flexcan%d.s", i);

        /* Board-to-board CAN: forward the per-controller canbus link (set from
         * `-machine canbus0=...,canbus1=...`) to the FlexCAN before realize. */
        if (s->canbus[i]) {
            object_property_set_link(OBJECT(&s->flexcan[i]), "canbus",
                                     OBJECT(s->canbus[i]), &error_abort);
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexcan[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexcan[i]), 0, can_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexcan[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            can_cfg[i].irq));
        memory_region_init_alias(&s->flexcan_s_alias[i], OBJECT(dev), aname,
                                 &s->flexcan[i].iomem, 0, MCXN_FLEXCAN_SIZE);
        memory_region_add_subregion(system_memory,
                                     can_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->flexcan_s_alias[i]);
    }

    /* ENET (Ethernet QoS): MAC/PHY-event interrupt to cpu0 NVIC.  0x2000 win.
     * Connect a host network backend (-nic) so real frames can flow; MAC
     * loopback mode still works without one. */
    qemu_configure_nic_device(DEVICE(&s->enet0), true, NULL);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->enet0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->enet0), 0, 0x40100000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->enet0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 139));
    memory_region_init_alias(&s->enet0_s_alias, OBJECT(dev), "mcxn.enet0.s",
                             &s->enet0.iomem, 0, MCXN_ENET_SIZE);
    memory_region_add_subregion(system_memory, 0x40100000 + MCXN_SECURE_ALIAS,
                                &s->enet0_s_alias);

    /* RTC (calendar): 1 Hz tick + alarm interrupt to cpu0 NVIC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc0), 0, 0x4004C000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 52));
    memory_region_init_alias(&s->rtc0_s_alias, OBJECT(dev), "mcxn.rtc0.s",
                             &s->rtc0.iomem, 0, MCXN_RTC_SIZE);
    memory_region_add_subregion(system_memory, 0x4004C000 + MCXN_SECURE_ALIAS,
                                &s->rtc0_s_alias);

    /* uSDHC (SD/MMC host): command/transfer-complete interrupt to cpu0 NVIC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usdhc0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usdhc0), 0, 0x40109000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usdhc0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 61));
    memory_region_init_alias(&s->usdhc0_s_alias, OBJECT(dev), "mcxn.usdhc0.s",
                             &s->usdhc0.iomem, 0, MCXN_USDHC_SIZE);
    memory_region_add_subregion(system_memory, 0x40109000 + MCXN_SECURE_ALIAS,
                                &s->usdhc0_s_alias);

    /* FlexSPI (external flash controller): IP-command-done IRQ to cpu0 NVIC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->flexspi0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexspi0), 0, 0x400C8000);

    /*
     * The board's external flash: a Winbond W25Q64 (8 MiB, JEDEC 0xef4017) on
     * the FlexSPI's SSI bus.  QEMU's m25p80 models this exact part, so the NOR
     * physics — erase-before-write, bits only 1 -> 0, page-program wrap, the WREN
     * latch — come from the upstream flash model instead of being re-invented in
     * the controller.  m25p80 is the sole authority for flash content; the AHB
     * XIP window is only a mirror derived from it (see hw/misc/mcxn_flexspi.c).
     */
    {
        DeviceState *nor = qdev_new("w25q64");

        qdev_realize_and_unref(nor, BUS(s->flexspi0.spi), &error_fatal);
        /* sysbus IRQ 0 is the flash chip-select; IRQ 1 is the NVIC line. */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexspi0), 0,
                           qdev_get_gpio_in_named(nor, SSI_GPIO_CS, 0));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexspi0), 1,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 58));

    memory_region_init_alias(&s->flexspi0_s_alias, OBJECT(dev), "mcxn.flexspi0.s",
                             &s->flexspi0.iomem, 0, MCXN_FLEXSPI_SIZE);
    memory_region_add_subregion(system_memory, 0x400C8000 + MCXN_SECURE_ALIAS,
                                &s->flexspi0_s_alias);
    /* FlexSPI0 AHB-mapped external NOR (XIP window): non-secure @ 0x8000_0000,
     * secure alias @ 0x9000_0000 (N947 DTS: spi@500c8000 ahb = 0x9000_0000,
     * +0x1000_0000 TZ-M offset).  Backs the window with real executable memory
     * so code linked there boots/runs in place instead of faulting. */
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexspi0), 1, MCXN_FLEXSPI0_AHB_NS);
    memory_region_init_alias(&s->flexspi0_nor_s_alias, OBJECT(dev),
                             "mcxn.flexspi0.nor.s", &s->flexspi0.nor, 0,
                             s->flexspi0.flash_size);
    memory_region_add_subregion(system_memory, MCXN_FLEXSPI0_AHB_S,
                                &s->flexspi0_nor_s_alias);

    /* USB device-mode: two independent controllers, each with its own usbredir
     * core (so each is a distinct inter-QEMU link).  Each core's socket is
     * attached by a well-known chardev id — `-chardev socket,id=mcxn-usbfs,...`
     * for USBFS0, `id=mcxn-usbhs,...` for USBHS1 — looked up here (absent = the
     * controller simply never appears on a host).  USBFS0: KHCI device engine,
     * NS @ 0x400D_D000 + secure alias, IRQ 50 (USB0_FS_IRQn).  USBHS1: ChipIdea
     * device engine, NS @ 0x4010_B000 + secure alias, IRQ 67 (USB1_HS_IRQn). */
    {
        Chardev *c0 = qemu_chr_find("mcxn-usbfs");
        Chardev *c1 = qemu_chr_find("mcxn-usbhs");
        if (c0) {
            qdev_prop_set_chr(DEVICE(&s->usbdev), "chardev", c0);
        }
        if (c1) {
            qdev_prop_set_chr(DEVICE(&s->usbdev_hs), "chardev", c1);
        }
    }
    if (!qdev_realize(DEVICE(&s->usbdev), NULL, errp)) {
        return;
    }
    if (!qdev_realize(DEVICE(&s->usbdev_hs), NULL, errp)) {
        return;
    }
    object_property_set_link(OBJECT(&s->usbfs0), "usbdev",
                             OBJECT(&s->usbdev), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usbfs0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usbfs0), 0, 0x400DD000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usbfs0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 50));
    memory_region_init_alias(&s->usbfs0_s_alias, OBJECT(dev), "mcxn.usbfs0.s",
                             &s->usbfs0.iomem, 0, MCXN_USBFS_SIZE);
    memory_region_add_subregion(system_memory, 0x400DD000 + MCXN_SECURE_ALIAS,
                                &s->usbfs0_s_alias);

    object_property_set_link(OBJECT(&s->usbhs_core), "usbdev",
                             OBJECT(&s->usbdev_hs), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usbhs_core), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usbhs_core), 0, 0x4010B000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usbhs_core), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 67));
    memory_region_init_alias(&s->usbhs_core_s_alias, OBJECT(dev),
                             "mcxn.usbhs-core.s", &s->usbhs_core.iomem, 0,
                             MCXN_USBHS_CORE_SIZE);
    memory_region_add_subregion(system_memory, 0x4010B000 + MCXN_SECURE_ALIAS,
                                &s->usbhs_core_s_alias);

    /* eIQ Neutron NPU: NS @ 0x400B_E000 + secure alias, IRQ 97 (RM NPU line;
     * "Reserved113" in CMSIS).  FLAG-AT-OPERATOR — see hw/misc/mcxn_neutron.c. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->neutron0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->neutron0), 0, 0x400BE000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->neutron0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 97));
    memory_region_init_alias(&s->neutron0_s_alias, OBJECT(dev), "mcxn.neutron0.s",
                             &s->neutron0.iomem, 0, MCXN_NEUTRON_SIZE);
    memory_region_add_subregion(system_memory, 0x400BE000 + MCXN_SECURE_ALIAS,
                                &s->neutron0_s_alias);

    /* SAI0..1 (audio): FIFO-request/error interrupt to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_SAI; i++) {
        static const struct { hwaddr base; int irq; }
        sai_cfg[MCXN_NUM_SAI] = { { 0x40106000, 59 }, { 0x40107000, 60 } };
        g_autofree char *aname = g_strdup_printf("mcxn.sai%d.s", i);
        g_autofree char *cn = g_strdup_printf("sai%d-clk", i);

        /* SAI function clock (MCLK) from SYSCON SAInCLKSEL/CLKDIV -- DERIVED (PLL0/ExtClk/
         * FRO_HF/PLL1), no longer a hardcoded 12.288 MHz.  Connect before realize. */
        qdev_connect_clock_in(DEVICE(&s->sai[i]), "clk",
                              qdev_get_clock_out(DEVICE(&s->syscon), cn));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->sai[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->sai[i]), 0, sai_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->sai[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            sai_cfg[i].irq));
        memory_region_init_alias(&s->sai_s_alias[i], OBJECT(dev), aname,
                                 &s->sai[i].iomem, 0, MCXN_SAI_SIZE);
        memory_region_add_subregion(system_memory,
                                     sai_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->sai_s_alias[i]);
    }

    /* DAC0..2: FIFO watermark/empty/error interrupt to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_DAC; i++) {
        static const struct { hwaddr base; int irq; }
        dac_cfg[MCXN_NUM_DAC] = { { 0x4010F000, 106 }, { 0x40112000, 107 },
                                  { 0x40114000, 108 } };
        g_autofree char *aname = g_strdup_printf("mcxn.dac%d.s", i);

        /* DAC2 is the HPDAC: 14-bit samples and a 32-deep FIFO, where DAC0/1
         * are 12-bit with a 16-deep one. */
        qdev_prop_set_bit(DEVICE(&s->dac[i]), "hpdac", i == 2);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->dac[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->dac[i]), 0, dac_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->dac[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            dac_cfg[i].irq));
        memory_region_init_alias(&s->dac_s_alias[i], OBJECT(dev), aname,
                                 &s->dac[i].iomem, 0, MCXN_DAC_SIZE);
        memory_region_add_subregion(system_memory,
                                     dac_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->dac_s_alias[i]);
    }

    /* SINC sigma-delta filter: conversion-complete / FIFO-watermark interrupt
     * to cpu0 NVIC (SINC_FILTER_IRQn = 142). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sinc0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sinc0), 0, 0x40108000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sinc0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 142));
    memory_region_init_alias(&s->sinc0_s_alias, OBJECT(dev), "mcxn.sinc0.s",
                             &s->sinc0.iomem, 0, MCXN_SINC_SIZE);
    memory_region_add_subregion(system_memory, 0x40108000 + MCXN_SECURE_ALIAS,
                                &s->sinc0_s_alias);

    /* PDM / MICFIL (digital microphone): FIFO/error interrupt to cpu0 NVIC
     * (PDM_EVENT_IRQn = 48). */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pdm0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pdm0), 0, 0x4010C000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pdm0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 48));
    memory_region_init_alias(&s->pdm0_s_alias, OBJECT(dev), "mcxn.pdm0.s",
                             &s->pdm0.iomem, 0, MCXN_PDM_SIZE);
    memory_region_add_subregion(system_memory, 0x4010C000 + MCXN_SECURE_ALIAS,
                                &s->pdm0_s_alias);

    /* PowerQuad (DSP coprocessor): compute-complete interrupt to cpu0 NVIC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->powerquad0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->powerquad0), 0, 0x400BF000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->powerquad0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 76));
    memory_region_init_alias(&s->powerquad0_s_alias, OBJECT(dev),
                             "mcxn.powerquad0.s", &s->powerquad0.iomem, 0,
                             MCXN_POWERQUAD_SIZE);
    memory_region_add_subregion(system_memory, 0x400BF000 + MCXN_SECURE_ALIAS,
                                &s->powerquad0_s_alias);

    /* eFlexPWM0..1: submodule-0 reload/compare interrupt to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_PWM; i++) {
        static const struct { hwaddr base; int irq; }
        pwm_cfg[MCXN_NUM_PWM] = { { 0x400CE000, 114 }, { 0x400D0000, 120 } };
        g_autofree char *aname = g_strdup_printf("mcxn.pwm%d.s", i);

        /* The FlexPWM counter is clocked by the bus clock = the SCG main clock, DERIVED
         * (48 MHz reset -> 150 MHz once firmware brings up PLL0) instead of a hardcoded
         * constant.  Connect before realize (qdev_connect_clock_in asserts !realized). */
        qdev_connect_clock_in(DEVICE(&s->pwm[i]), "clk",
                              qdev_get_clock_out(DEVICE(&s->syscon), "busclk"));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwm[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwm[i]), 0, pwm_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->pwm[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            pwm_cfg[i].irq));
        memory_region_init_alias(&s->pwm_s_alias[i], OBJECT(dev), aname,
                                 &s->pwm[i].iomem, 0, MCXN_PWM_SIZE);
        memory_region_add_subregion(system_memory,
                                     pwm_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->pwm_s_alias[i]);
    }

    /* SCT (SCTimer/PWM): match/limit event interrupt to cpu0 NVIC. */
    /*
     * The SCT's rate is DECIDED by SYSCON[SCTCLKSEL]/[SCTCLKDIV].  It was hardwired to
     * a 150 MHz constant while its own source comment NAMED those very registers --
     * and every stock example does CLOCK_AttachClk(kFRO_HF_to_SCT), 48 MHz.  3.1x too
     * fast, and the test could not see it because the test shared the assumption.
     */
    qdev_connect_clock_in(DEVICE(&s->sct0), "clk",
                          qdev_get_clock_out(DEVICE(&s->syscon), "sct-clk"));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sct0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sct0), 0, 0x40091000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sct0), 0,
                       qdev_get_gpio_in(DEVICE(&s->armv7m[0]), 33));
    memory_region_init_alias(&s->sct0_s_alias, OBJECT(dev), "mcxn.sct0.s",
                             &s->sct0.iomem, 0, MCXN_SCT_SIZE);
    memory_region_add_subregion(system_memory, 0x40091000 + MCXN_SECURE_ALIAS,
                                &s->sct0_s_alias);

    /* I3C0..1: controller transfer-complete interrupt to cpu0 NVIC. */
    for (i = 0; i < MCXN_NUM_I3C; i++) {
        static const struct { hwaddr base; int irq; }
        i3c_cfg[MCXN_NUM_I3C] = { { 0x40021000, 95 }, { 0x40022000, 96 } };
        g_autofree char *aname = g_strdup_printf("mcxn.i3c%d.s", i);

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->i3c[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->i3c[i]), 0, i3c_cfg[i].base);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->i3c[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m[0]),
                                            i3c_cfg[i].irq));
        memory_region_init_alias(&s->i3c_s_alias[i], OBJECT(dev), aname,
                                 &s->i3c[i].iomem, 0, MCXN_I3C_SIZE);
        memory_region_add_subregion(system_memory,
                                     i3c_cfg[i].base + MCXN_SECURE_ALIAS,
                                     &s->i3c_s_alias[i]);
    }

    /* OSTIMER (OS event timer): 1 MHz default clock, match IRQ to cpu0 NVIC. */
    /*
     * The OSTIMER's rate is DECIDED by SYSCON[OSTIMERCLKSEL] -- 16k / 32k / 1M / none.
     * The device DECLARED this Clock input and the SoC NEVER CONNECTED IT, so the
     * device fell back to a hardcoded 1 MHz and the selector did nothing at all.
     *
     * ⚠ qdev_connect_clock_in() ASSERTS !dev->realized -- it must run BEFORE the
     * target is realized, which is the MIRROR IMAGE of the GPIO rule
     * (qdev_get_gpio_in() needs the target ALREADY realized).  The two constraints
     * point in OPPOSITE directions, and getting it wrong the other way is what
     * silently dropped the peripheral DMA request wiring once already.
     */
    qdev_connect_clock_in(DEVICE(&s->ostimer0), "clk",
                          qdev_get_clock_out(DEVICE(&s->syscon), "ostimer-clk"));

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

    /* FMU flash controller.  The flash backing was handed to it above (it owns
     * the ROM-device region, so guest stores land in its program/erase state
     * machine); here we just map its registers and IRQ to the cpu0 NVIC. */
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

    /*
     * ═══ PERIPHERAL DMA REQUEST LINES — ALL OF THEM, HERE, LAST ═══
     *
     * ⚠ qdev_get_gpio_in() only works once the target is REALIZED, and
     * sysbus_connect_irq() only works once the SOURCE is realized.  So this
     * wiring is order-dependent in BOTH directions, and I got burned BOTH ways:
     *
     *   - FlexComm realizes BEFORE the eDMA, so wiring it in its own loop
     *     silently connected NOTHING (SAI and DAC only worked by accident, being
     *     declared after the eDMA);
     *   - then I moved the wiring to just after the eDMA -- and the ADC realizes
     *     AFTER that, so QEMU aborted outright:
     *         "Property 'mcxn-adc.sysbus-irq[1]' not found".
     *
     * Two opposite failures from the same cause.  Doing every request line HERE,
     * at the END, after EVERY peripheral exists, removes the dependency instead of
     * tiptoeing around it.  Add new request lines to THIS block and nowhere else.
     *
     * Source numbers are CMSIS dma_request_source_t.
     */
    for (i = 0; i < MCXN_NUM_ADC; i++) {                  /* FIFO A/B = 21+2n, 22+2n */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc[i]), 1,
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_ADC0_FIFO_A + 2 * i));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc[i]), 2,
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_ADC0_FIFO_B + 2 * i));
    }
    for (i = 0; i < MCXN_NUM_DAC; i++) {                  /* DAC0/1/2 = 25/26/27 */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->dac[i]), 1,
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_DAC0_FIFO + i));
    }
    for (i = 0; i < MCXN_NUM_SAI; i++) {                  /* SAI Tx=100+2n, Rx=99+2n */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->sai[i]), 1,
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_SAI0_TX + 2 * i));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->sai[i]), 2,
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_SAI0_RX + 2 * i));
    }
    for (i = 0; i < MCXN_SINC_NUM_CH; i++) {             /* SINC0 ch n -> 103+n */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->sinc0), 1 + i,   /* sysbus IRQ 0 is the NVIC line */
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_SINC0_CH0 + i));
    }
    /* FlexSPI0: sysbus IRQ 2 = Rx (src 1), IRQ 3 = Tx (src 2).  IRQ 0/1 are cs/NVIC. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexspi0), 2,
                       qdev_get_gpio_in(DEVICE(&s->edma[0]), MCXN_DMA_REQ_FLEXSPI0_RX));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexspi0), 3,
                       qdev_get_gpio_in(DEVICE(&s->edma[0]), MCXN_DMA_REQ_FLEXSPI0_TX));
    /* eFlexPWM0/1: sysbus IRQ 1 = SM0 value-register DMA request (reload-driven).
     * PWM0 SM0 Val0 = 43, PWM1 SM0 Val0 = 51.  IRQ 0 is the NVIC reload/compare line. */
    {
        static const int pwm_val_src[MCXN_NUM_PWM] = {
            MCXN_DMA_REQ_FLEXPWM0_VAL0, MCXN_DMA_REQ_FLEXPWM1_VAL0
        };
        static const int pwm_cap_src[MCXN_NUM_PWM] = {
            MCXN_DMA_REQ_FLEXPWM0_CAP0, MCXN_DMA_REQ_FLEXPWM1_CAP0
        };
        for (i = 0; i < MCXN_NUM_PWM; i++) {
            /* IRQ 1 = value-reg DMA (a FIFO-ish reload level); IRQ 2 = SM0 input-A capture
             * DMA (a one-shot PULSE, so it drives the eDMA "req-pulse" input). */
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->pwm[i]), 1,
                               qdev_get_gpio_in(DEVICE(&s->edma[0]), pwm_val_src[i]));
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->pwm[i]), 2,
                               qdev_get_gpio_in_named(DEVICE(&s->edma[0]), "req-pulse",
                                                      pwm_cap_src[i]));
        }
    }
    /* CTIMER0..4: sysbus IRQ 1 = match-0 request, IRQ 2 = match-1 request.  These are
     * one-shot PULSE sources (a match is an event, not a FIFO level), so they drive the
     * eDMA's "req-pulse" input, not the plain level input.  CTIMER{k} M0 = 7+2k, M1 = 8+2k. */
    for (i = 0; i < MCXN_NUM_CTIMER; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->ctimer[i]), 1,
                           qdev_get_gpio_in_named(DEVICE(&s->edma[0]), "req-pulse",
                                                  MCXN_DMA_REQ_CTIMER0_M0 + 2 * i));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->ctimer[i]), 2,
                           qdev_get_gpio_in_named(DEVICE(&s->edma[0]), "req-pulse",
                                                  MCXN_DMA_REQ_CTIMER0_M1 + 2 * i));
    }
    /* SCT0: sysbus IRQ 1 = DMA request 0 (src 19), IRQ 2 = DMA request 1 (src 20).  An SCT
     * event is a one-shot PULSE, so these drive the eDMA "req-pulse" input.  IRQ 0 is NVIC. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sct0), 1,
                       qdev_get_gpio_in_named(DEVICE(&s->edma[0]), "req-pulse",
                                              MCXN_DMA_REQ_SCT0_DMA0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sct0), 2,
                       qdev_get_gpio_in_named(DEVICE(&s->edma[0]), "req-pulse",
                                              MCXN_DMA_REQ_SCT0_DMA1));
    /* CMP0..2 (HsCmp): sysbus IRQ 1 = DMA request (src 28+n).  A comparator crossing is a
     * one-shot PULSE, so it drives the eDMA "req-pulse" input.  IRQ 0 is the NVIC line. */
    for (i = 0; i < MCXN_NUM_CMP; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->cmp[i]), 1,
                           qdev_get_gpio_in_named(DEVICE(&s->edma[0]), "req-pulse",
                                                  MCXN_DMA_REQ_HSCMP0 + i));
    }
    /* PINT INT0..3: sysbus IRQ 1..4 = DMA request (src 3+n).  A pin edge is a one-shot
     * PULSE, so it drives the eDMA "req-pulse" input.  IRQ 0 is the shared NVIC line. */
    for (i = 0; i < MCXN_PINT_DMA_LINES; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->pint0), 1 + i,
                           qdev_get_gpio_in_named(DEVICE(&s->edma[0]), "req-pulse",
                                                  MCXN_DMA_REQ_PINT0 + i));
    }
    /* PDM/MICFIL0: sysbus IRQ 1 = FIFO DMA request (src 18).  A FIFO watermark is a LEVEL
     * (like the SAI), so it drives the plain eDMA request input.  IRQ 0 is the NVIC line. */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pdm0), 1,
                       qdev_get_gpio_in(DEVICE(&s->edma[0]), MCXN_DMA_REQ_MICFIL0));
    for (i = 0; i < MCXN_NUM_FLEXCOMM && i < 10; i++) {   /* Tx=70+2n, Rx=69+2n */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexcomm[i]), 1,
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_LPFLEXCOMM0_TX + 2 * i));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexcomm[i]), 2,
                           qdev_get_gpio_in(DEVICE(&s->edma[0]),
                                            MCXN_DMA_REQ_LPFLEXCOMM0_RX + 2 * i));
    }

    /*
     * INPUTMUX gates every one of those request lines.
     *
     * DMAn_REQ_ENABLE0..3 is one bit per request source, and it decides whether a
     * peripheral's request reaches the engine AT ALL (RM: "0: DMA request to DMA0
     * and response from DMA0 are blocked").  It resets to all-enabled, which is why
     * nothing broke while it was unmodelled -- and also why it was worth modelling:
     * an UNGATED model is MORE PERMISSIVE THAN THE SILICON, so a guest that CLOSES a
     * gate sees the request keep coming.  That is the silent-wrong-answer class
     * inverted: it does not fail here, IT FAILS ON THE BOARD.
     *
     * Wired last, with the rest of the DMA request lines, because
     * qdev_get_gpio_in_named() needs the TARGET realized and qdev_connect_gpio_out
     * needs the SOURCE realized -- an ordering constraint in both directions.
     */
    for (i = 0; i < MCXN_NUM_EDMA; i++) {
        g_autofree char *gate = g_strdup_printf("dma%d-req-enable", i);
        int src;

        for (src = 0; src < MCXN_EDMA_REQ_SOURCES; src++) {
            qdev_connect_gpio_out_named(DEVICE(&s->inputmux), gate, src,
                                        qdev_get_gpio_in_named(
                                            DEVICE(&s->edma[i]), "req-enable",
                                            src));
        }
    }

    /*
     * TRIGGER ROUTING:  LPTMR0 compare  ->  INPUTMUX  ->  ADCn_TRIG[0..3].
     *
     * "Convert on a timer tick" is THE canonical embedded ADC pattern, and it was
     * IMPOSSIBLE here: the only way to start a conversion was a CPU write to SWTRIG.
     * The stock lpadc/edma example attaches LPTMR0 to ADC0_TRIG[0], starts the timer,
     * arms an eDMA channel and waits -- and the timer ticked, THE TRIGGER WENT
     * NOWHERE, and not one conversion ever happened.  Nothing logged, nothing
     * faulted.  A router that routes nothing looks exactly like a router.
     *
     * Selector 50 = LPTMR0.  Decoded from NXP's OWN COMPILED DRIVER, not guessed:
     * kINPUTMUX_Lptmr0ToAdc0Trigger = 0x2800_0032, and INPUTMUX_AttachSignal() is
     *     *(base + (conn >> 20) + idx * 4) = conn & 0xFFFFF
     * i.e. "write 50 into the register at offset 0x280" -- which is ADC0_TRIG[0].
     */
    qdev_connect_gpio_out_named(DEVICE(&s->lptmr[0]), "trigger", 0,
                                qdev_get_gpio_in_named(DEVICE(&s->inputmux),
                                                       "trig-in",
                                                       MCXN_INPUTMUX_SRC_LPTMR0));
    for (i = 0; i < MCXN_NUM_ADC && i < 2; i++) {
        g_autofree char *out = g_strdup_printf("adc%d-trig", i);
        int t;

        for (t = 0; t < 4; t++) {
            qdev_connect_gpio_out_named(DEVICE(&s->inputmux), out, t,
                                        qdev_get_gpio_in_named(
                                            DEVICE(&s->adc[i]), "trigger", t));
        }
    }
}

static const Property mcxn_soc_properties[] = {
    DEFINE_PROP_STRING("part", MCXNState, part),
    DEFINE_PROP_BOOL("qspi-boot", MCXNState, qspi_boot, false),
    DEFINE_PROP_LINK("canbus0", MCXNState, canbus[0], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus1", MCXNState, canbus[1], TYPE_CAN_BUS,
                     CanBusState *),
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
