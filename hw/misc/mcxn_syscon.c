/*
 * NXP MCX N SYSCON — secondary-core (CPU1) boot control (partial model)
 *
 * On the dual-Cortex-M33 MCXN947, the primary core (cpu0) releases the
 * secondary core (cpu1) by writing its vector-table base to SYSCON.CPBOOT and
 * enabling it via SYSCON.CPUCTRL (CPU1CLKEN set, CPU1RSTEN cleared).  This
 * device models exactly that handover and backs the remaining SYSCON registers
 * permissively so early firmware clock/reset init does not fault.
 *
 * Register offsets and bit fields are from the MCXN947 CMSIS header
 * (devices/MCXN947/MCXN947_cm33_core0.h): SYSCON CPUCTRL @0x800, CPBOOT @0x804.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "hw/misc/mcxn_syscon.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "target/arm/cpu.h"

/* --- SYSCON register offsets (CMSIS) --------------------------------------- */
#define SYSCON_CPUCTRL  0x800   /* CPU Control for Multiple Processors */
#define SYSCON_CPBOOT   0x804   /* Coprocessor (CPU1) Boot Address     */
#define SYSCON_CPSTAT   0x808   /* CPU Status                          */

/* --- CPUCTRL fields (CMSIS) ------------------------------------------------ */
#define CPUCTRL_CPU1CLKEN  (1u << 3)   /* SYSCON_CPUCTRL_CPU1CLKEN_MASK 0x8  */
#define CPUCTRL_CPU1RSTEN  (1u << 5)   /* SYSCON_CPUCTRL_CPU1RSTEN_MASK 0x20 */

/* CPBOOT holds the CPU1 vector-table (VTOR) base in bits [31:7]. */
#define CPBOOT_ADDR_MASK   0xFFFFFF80u

/* CPSTAT status bits (CMSIS): CPU1 sleeping. */
#define CPSTAT_CPU1SLEEPING (1u << 1)

/*
 * Keep cs->halted, the PSCI power_state and env->halt_reason consistent at
 * every start/stop.  After the WFI/WFE halt-reason rework in the QEMU base,
 * arm_cpu_has_work() asserts that a PSCI_OFF core is HALT_PSCI, so a running
 * core left at PSCI_OFF trips that assert on its first WFI.
 */
static void mcxn_syscon_set_cpu1_run(ARMCPU *cpu, bool run)
{
    CPUState *cs = CPU(cpu);

    cs->halted = !run;
    cpu->power_state = run ? PSCI_ON : PSCI_OFF;
    cpu->env.halt_reason = run ? NOT_HALTED : HALT_PSCI;
}

static void mcxn_syscon_start_cpu1_bh(void *opaque)
{
    MCXNSysconState *s = opaque;
    ARMCPU *cpu = s->cpu1;

    if (!cpu) {
        return;
    }
    /*
     * Boot CPU1 from the vector table SYSCON.CPBOOT points at: set its
     * init-SVTOR, reset so M-profile reload reads SP/PC from that table, then
     * release it.  Deferred to a BH because a store from cpu0's TCG block must
     * not retune another vCPU inline.
     */
    cpu->init_svtor = s->cpboot & CPBOOT_ADDR_MASK;
    cpu_reset(CPU(cpu));
    mcxn_syscon_set_cpu1_run(cpu, true);
    cpu_resume(CPU(cpu));
    s->cpu1_running = true;
}

static void mcxn_syscon_update_cpu1(MCXNSysconState *s)
{
    bool want_run = (s->cpuctrl & CPUCTRL_CPU1CLKEN) &&
                    !(s->cpuctrl & CPUCTRL_CPU1RSTEN);

    if (!s->cpu1) {
        return;
    }
    if (want_run && !s->cpu1_running) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                mcxn_syscon_start_cpu1_bh, s);
    } else if (!want_run && s->cpu1_running) {
        mcxn_syscon_set_cpu1_run(s->cpu1, false);
        s->cpu1_running = false;
    }
}

static uint64_t mcxn_syscon_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNSysconState *s = MCXN_SYSCON(opaque);

    switch (offset) {
    case SYSCON_CPUCTRL:
        return s->cpuctrl;
    case SYSCON_CPBOOT:
        return s->cpboot;
    case SYSCON_CPSTAT:
        return s->cpu1_running ? 0 : CPSTAT_CPU1SLEEPING;
    default:
        return s->regs[offset / 4];
    }
}

static void mcxn_syscon_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    MCXNSysconState *s = MCXN_SYSCON(opaque);

    switch (offset) {
    case SYSCON_CPUCTRL:
        /* The PROT key in [31:16] gates writes on HW; modelled permissively. */
        s->cpuctrl = value;
        mcxn_syscon_update_cpu1(s);
        break;
    case SYSCON_CPBOOT:
        s->cpboot = value;
        break;
    default:
        s->regs[offset / 4] = value;
        qemu_log_mask(LOG_UNIMP, "%s: unmodelled SYSCON write @0x%03" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps mcxn_syscon_ops = {
    .read = mcxn_syscon_read,
    .write = mcxn_syscon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_syscon_reset(DeviceState *dev)
{
    MCXNSysconState *s = MCXN_SYSCON(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->cpuctrl = 0;
    s->cpboot = 0;
    s->cpu1_running = false;
}

static void mcxn_syscon_realize(DeviceState *dev, Error **errp)
{
    MCXNSysconState *s = MCXN_SYSCON(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_syscon_ops, s,
                          TYPE_MCXN_SYSCON, MCXN_SYSCON_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const VMStateDescription vmstate_mcxn_syscon = {
    .name = TYPE_MCXN_SYSCON,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSysconState, MCXN_SYSCON_SIZE / 4),
        VMSTATE_UINT32(cpuctrl, MCXNSysconState),
        VMSTATE_UINT32(cpboot, MCXNSysconState),
        VMSTATE_BOOL(cpu1_running, MCXNSysconState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mcxn_syscon_properties[] = {
    DEFINE_PROP_LINK("cpu1", MCXNSysconState, cpu1, TYPE_ARM_CPU, ARMCPU *),
};

static void mcxn_syscon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_syscon_realize;
    device_class_set_legacy_reset(dc, mcxn_syscon_reset);
    dc->vmsd = &vmstate_mcxn_syscon;
    device_class_set_props(dc, mcxn_syscon_properties);
}

static const TypeInfo mcxn_syscon_types[] = {
    {
        .name          = TYPE_MCXN_SYSCON,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSysconState),
        .class_init    = mcxn_syscon_class_init,
    },
};

DEFINE_TYPES(mcxn_syscon_types)
