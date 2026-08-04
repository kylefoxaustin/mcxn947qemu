/*
 * NXP MCX N SMARTDMA (programmable DMA coprocessor) - bring-up model.  See
 * header for design notes.  Offsets and bits from the MCXN947 CMSIS header
 * (SMARTDMA_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/mcxn_smartdma.h"
#include "hw/core/irq.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "system/dma.h"

/* Register offsets (CMSIS SMARTDMA_Type). */
#define R_BOOTADR     0x20  /* RW: boot address (firmware jump-table entry) */
#define R_CTRL        0x24  /* RW: control (keyed 0xC0DE; boot in bit 0) */
#define R_PC          0x28  /* RO: program counter */
#define R_SP          0x2C  /* RO: stack pointer */
#define R_BREAK_ADDR  0x30  /* RW */
#define R_BREAK_VECT  0x34  /* RW */
#define R_EMER_VECT   0x38  /* RW */
#define R_EMER_SEL    0x3C  /* RW */
#define R_ARM2EZH     0x40  /* RW: ARM->EZH — carries pParam | mask on boot */
#define R_EZH2ARM     0x44  /* RW: EZH-to-ARM trigger (W1C-style status) */
#define R_PENDTRAP    0x48  /* RW: pending trap control (STATUS is W1C) */

/*
 * CTRL is a KEYED register: the SDK driver (fsl_smartdma.c) always writes
 * 0xC0DE_00xx — 0xC0DE in [31:16] is the write key, the command lives in the
 * low 16 bits.  Boot = bit0 (HANDSHAKE_EVENT), GPISYNCH = bit4.  A keyless
 * write is not a valid command on silicon.
 */
#define CTRL_KEY        0xC0DE0000u
#define CTRL_KEY_MASK   0xFFFF0000u
#define CTRL_CMD_MASK   0x0000FFFFu
#define CTRL_START      (1u << 0)  /* boot ignition — HONEST-FAULT: stays set */
#define CTRL_GPISYNCH   (1u << 4)

/* ARM2EZH low 2 bits = mask; the rest is the (word-aligned) pParam pointer. */
#define ARM2EZH_MASK    0x3u

/* PENDTRAP.STATUS is the pending-trap request flag set (W1C). */
#define PENDTRAP_STATUS_MASK  0xFFu

/*
 * The SDK installs its firmware at SRAMX (SMARTDMA_DISPLAY_MEM_ADDR /
 * SMARTDMA_CAMERA_MEM_ADDR, both 0x0400_0000).  The blob's first words are a
 * jump table: s_smartdmaApiTable[apiIndex] = *(u32*)(base + apiIndex*4), and
 * SMARTDMA_Boot() writes that resolved entry to BOOTADR.  So we recover the
 * requested apiIndex by scanning the installed table for the booted entry.
 */
#define SMARTDMA_FW_BASE   0x04000000u
#define SMARTDMA_FW_SLOTS  16
/* The MCXN display firmware's table starts with this entry (fsl_smartdma_mcxn.c
 * s_smartdmaDisplayFirmware[0..3] = 0x04000024) — a fingerprint, not a fake. */
#define SMARTDMA_DISPLAY_FW0  0x04000024u

/* Documented display-firmware API names (enum _smartdma_display_api). */
static const char *const smartdma_display_api[] = {
    "FlexIO_DMA_Endian_Swap",
    "FlexIO_DMA_Reverse32",
    "FlexIO_DMA",
    "FlexIO_DMA_Reverse",
    "RGB565To888",
    "FlexIO_DMA_RGB565To888",
    "FlexIO_DMA_ARGB2RGB",
    "FlexIO_DMA_ARGB2RGB_Endian_Swap",
    "FlexIO_DMA_ARGB2RGB_Endian_Swap_Reverse",
};

/*
 * Decode a keyed CTRL boot and emit an INFORMATIVE honest-fault: name the exact
 * documented operation the guest asked for, but run nothing and move nothing.
 * The EZH is proprietary microcode with no ISA in the RM and no reference
 * implementation, so its transforms cannot be reproduced byte-exact — faking
 * them would be a silent wrong answer.  We therefore leave START set (the engine
 * never completes) and raise no completion IRQ, exactly as before; the only
 * change is that the diagnostic now names WHICH op was requested.
 */
static void mcxn_smartdma_boot(MCXNSmartDMAState *s)
{
    uint32_t bootadr = s->regs[R_BOOTADR >> 2];
    uint32_t arm2ezh = s->regs[R_ARM2EZH >> 2];
    uint32_t pparam = arm2ezh & ~ARM2EZH_MASK;
    uint32_t mask = arm2ezh & ARM2EZH_MASK;
    uint32_t fw0 = address_space_ldl_le(&address_space_memory, SMARTDMA_FW_BASE,
                                        MEMTXATTRS_UNSPECIFIED, NULL);
    bool display_fw = (fw0 == SMARTDMA_DISPLAY_FW0);
    int api = -1;
    const char *opname = "unrecognised firmware";
    int i;

    for (i = 0; i < SMARTDMA_FW_SLOTS; i++) {
        uint32_t e = address_space_ldl_le(&address_space_memory,
                                          SMARTDMA_FW_BASE + (i * 4),
                                          MEMTXATTRS_UNSPECIFIED, NULL);
        if (e == bootadr) {
            api = i;
            break;
        }
    }
    if (display_fw && api >= 0 && api < (int)ARRAY_SIZE(smartdma_display_api)) {
        opname = smartdma_display_api[api];
    }

    s->last_bootadr = bootadr;
    s->last_pparam = pparam;
    s->last_mask = mask;
    s->last_apiindex = (api >= 0) ? (uint32_t)api : 0xFFFFFFFFu;
    s->programs_started++;

    qemu_log_mask(LOG_UNIMP,
                  "%s: SmartDMA BOOT requested — op=%s (apiIndex=%d, "
                  "bootADR=0x%08x), pParam=0x%08x mask=%u. The EZH core is NOT "
                  "modelled (proprietary microcode, no ISA in the RM, no "
                  "reference impl), so the program does NOT run: NOTHING IS "
                  "MOVED, no completion IRQ, START stays set — rather than "
                  "faking a transform (silent wrong answer) or reporting a "
                  "completion that never happened.  compute-modelled=false "
                  "programs-started=%u\n",
                  __func__, opname, api, bootadr, pparam, mask,
                  s->programs_started);
}

static uint64_t mcxn_smartdma_read(void *opaque, hwaddr off, unsigned size)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(opaque);
    uint32_t v = (off < MCXN_SMARTDMA_SIZE) ? s->regs[off >> 2] : 0;

    switch (off) {
    case R_CTRL:
        /*
         * START reads back exactly as the engine's real state: still SET, because
         * the EZH program was never executed and therefore never completed.
         *
         * Clearing it (the old behaviour) told a polling guest "your transfer is
         * done" while the destination buffer had never been touched — a silent
         * wrong answer, and the worst kind, because SmartDMA's whole job is to
         * MOVE DATA to a pointer the guest gave us.  A boot-then-poll loop that
         * never falls through is a dead coprocessor, which is exactly what this
         * is; the guest can see that, and a LOG_UNIMP says why.
         */
        return v;
    case R_PC:
    case R_SP:
        /* Read-only engine state; nothing to model, report the boot address. */
        return s->regs[R_BOOTADR >> 2];
    default:
        return v;
    }
}

static void mcxn_smartdma_write(void *opaque, hwaddr off,
                                uint64_t value, unsigned size)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(opaque);
    uint32_t v = value;

    if (off >= MCXN_SMARTDMA_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: OOB write @0x%" HWADDR_PRIx "\n",
                      __func__, off);
        return;
    }

    switch (off) {
    case R_PC:
    case R_SP:
        return;                 /* read-only engine state */
    case R_CTRL:
        /*
         * CTRL is keyed (0xC0DE in [31:16]).  A keyless write is not a valid
         * command on silicon — surface that to the guest/operator and do not
         * act on it.  We store only the low 16 command bits for read-back, so
         * a booted START (bit0) reads back SET: the engine never completes.
         */
        if ((v & CTRL_KEY_MASK) != CTRL_KEY) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: CTRL write 0x%08x lacks the 0xC0DE key — the "
                          "SmartDMA driver always writes a keyed command; the "
                          "control bits are ignored.\n", __func__, v);
            s->regs[off >> 2] = v & CTRL_CMD_MASK;
            return;
        }
        s->regs[off >> 2] = v & CTRL_CMD_MASK;  /* strip key; START stays set */
        if (v & CTRL_START) {
            mcxn_smartdma_boot(s);              /* informative honest-fault */
        }
        /* GPISYNCH-only (init/enable) or 0x0 (stop) carry no program to run. */
        return;
    case R_EZH2ARM:
        /*
         * Writing EZH2ARM is the engine-to-ARM trigger; when ARM2EZH[1:0] == 2h
         * it would raise the ARM interrupt.  Latch the value but keep the line
         * deasserted (no real engine is running to drive it).
         */
        s->regs[off >> 2] = v;
        qemu_set_irq(s->irq, 0);
        return;
    case R_PENDTRAP:
        /* STATUS field (bits 7:0) is write-1-to-clear; other fields reflect. */
        s->regs[off >> 2] = (s->regs[off >> 2] & ~(v & PENDTRAP_STATUS_MASK))
                          | (v & ~PENDTRAP_STATUS_MASK);
        return;
    default:
        s->regs[off >> 2] = v;
        return;
    }
}

static const MemoryRegionOps mcxn_smartdma_ops = {
    .read = mcxn_smartdma_read,
    .write = mcxn_smartdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mcxn_smartdma_reset(DeviceState *dev)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->programs_started = 0;
    s->last_bootadr = 0;
    s->last_apiindex = 0xFFFFFFFFu;
    s->last_pparam = 0;
    s->last_mask = 0;
    qemu_set_irq(s->irq, 0);
}

/* Honesty marker: the SmartDMA EZH program is never executed. */
static bool mcxn_smartdma_compute_modelled(Object *obj, Error **errp)
{
    return false;
}

static void mcxn_smartdma_init(Object *obj)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(obj);

    /* Farm-control-plane visibility (qom-get): is the engine actually computing,
     * and how many program starts were acked-but-not-executed. */
    object_property_add_bool(obj, "compute-modelled",
                             mcxn_smartdma_compute_modelled, NULL);
    object_property_add_uint32_ptr(obj, "programs-started",
                                   &s->programs_started, OBJ_PROP_FLAG_READ);
    /* Last decoded boot: which documented op an un-run engine was asked for. */
    object_property_add_uint32_ptr(obj, "last-apiindex",
                                   &s->last_apiindex, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "last-bootadr",
                                   &s->last_bootadr, OBJ_PROP_FLAG_READ);
}

static void mcxn_smartdma_realize(DeviceState *dev, Error **errp)
{
    MCXNSmartDMAState *s = MCXN_SMARTDMA(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_smartdma_ops, s,
                          TYPE_MCXN_SMARTDMA, MCXN_SMARTDMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_mcxn_smartdma = {
    .name = TYPE_MCXN_SMARTDMA,
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, MCXNSmartDMAState, MCXN_SMARTDMA_SIZE / 4),
        VMSTATE_UINT32(programs_started, MCXNSmartDMAState),
        VMSTATE_UINT32(last_bootadr, MCXNSmartDMAState),
        VMSTATE_UINT32(last_apiindex, MCXNSmartDMAState),
        VMSTATE_UINT32(last_pparam, MCXNSmartDMAState),
        VMSTATE_UINT32(last_mask, MCXNSmartDMAState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_smartdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_smartdma_realize;
    device_class_set_legacy_reset(dc, mcxn_smartdma_reset);
    dc->vmsd = &vmstate_mcxn_smartdma;
}

static const TypeInfo mcxn_smartdma_types[] = {
    {
        .name          = TYPE_MCXN_SMARTDMA,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNSmartDMAState),
        .instance_init = mcxn_smartdma_init,
        .class_init    = mcxn_smartdma_class_init,
    },
};

DEFINE_TYPES(mcxn_smartdma_types)
