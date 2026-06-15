/*
 * NXP MCX N GPIO controller (Rapid GPIO) — functional model.
 *
 * Models output (PDOR + PSOR/PCOR/PTOR), direction (PDDR) and input (PDIR).
 * Output pins drive their qemu_irq output line; PDIR returns the driven output
 * level for output pins and the external input level for input pins.  Pin
 * interrupts (ICR/ISFR) are not modelled yet.
 *
 * Register offsets/bit semantics from the MCXN947 CMSIS header (GPIO_Type).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/gpio/mcxn_gpio.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

/* --- GPIO register offsets (CMSIS) ----------------------------------------- */
#define GPIO_VERID  0x00  /* RO */
#define GPIO_PARAM  0x04  /* RO */
#define GPIO_LOCK   0x0C
#define GPIO_PDOR   0x40  /* Port Data Output           */
#define GPIO_PSOR   0x44  /* Port Set Output    (W1S)   */
#define GPIO_PCOR   0x48  /* Port Clear Output  (W1C)   */
#define GPIO_PTOR   0x4C  /* Port Toggle Output (W1T)   */
#define GPIO_PDIR   0x50  /* Port Data Input    (RO)    */
#define GPIO_PDDR   0x54  /* Port Data Direction        */
#define GPIO_PIDR   0x58  /* Port Input Disable         */

#define GPIO_VERID_VALUE  0x01000000u
#define GPIO_PARAM_VALUE  MCXN_GPIO_PINS   /* pin count in low bits */

/* Drive each output line to its PDOR bit (output pins only). */
static void mcxn_gpio_update_outputs(MCXNGPIOState *s)
{
    int i;

    for (i = 0; i < MCXN_GPIO_PINS; i++) {
        if (s->pddr & (1u << i)) {
            qemu_set_irq(s->output[i], (s->pdor >> i) & 1);
        }
    }
}

/* Input data: output pins read back their driven level, input pins the
 * externally driven level. */
static uint32_t mcxn_gpio_pdir(MCXNGPIOState *s)
{
    return (s->pdor & s->pddr) | (s->in_level & ~s->pddr);
}

static void mcxn_gpio_set_input(void *opaque, int line, int level)
{
    MCXNGPIOState *s = MCXN_GPIO(opaque);

    if (level) {
        s->in_level |= (1u << line);
    } else {
        s->in_level &= ~(1u << line);
    }
}

static uint64_t mcxn_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    MCXNGPIOState *s = MCXN_GPIO(opaque);

    switch (offset) {
    case GPIO_VERID:
        return GPIO_VERID_VALUE;
    case GPIO_PARAM:
        return GPIO_PARAM_VALUE;
    case GPIO_LOCK:
        return s->lock;
    case GPIO_PDOR:
        return s->pdor;
    case GPIO_PDIR:
        return mcxn_gpio_pdir(s);
    case GPIO_PDDR:
        return s->pddr;
    case GPIO_PIDR:
        return s->pidr;
    case GPIO_PSOR:
    case GPIO_PCOR:
    case GPIO_PTOR:
        return 0;   /* write-only set/clear/toggle aliases read as 0 */
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled read @0x%03" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void mcxn_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    MCXNGPIOState *s = MCXN_GPIO(opaque);

    switch (offset) {
    case GPIO_LOCK:
        s->lock = value;
        break;
    case GPIO_PDOR:
        s->pdor = value;
        mcxn_gpio_update_outputs(s);
        break;
    case GPIO_PSOR:
        s->pdor |= (uint32_t)value;
        mcxn_gpio_update_outputs(s);
        break;
    case GPIO_PCOR:
        s->pdor &= ~(uint32_t)value;
        mcxn_gpio_update_outputs(s);
        break;
    case GPIO_PTOR:
        s->pdor ^= (uint32_t)value;
        mcxn_gpio_update_outputs(s);
        break;
    case GPIO_PDDR:
        s->pddr = value;
        mcxn_gpio_update_outputs(s);
        break;
    case GPIO_PIDR:
        s->pidr = value;
        break;
    case GPIO_VERID:
    case GPIO_PARAM:
    case GPIO_PDIR:
        break;  /* read-only */
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unhandled write @0x%03" HWADDR_PRIx
                      " = 0x%08x\n", __func__, offset, (uint32_t)value);
        break;
    }
}

static const MemoryRegionOps mcxn_gpio_ops = {
    .read = mcxn_gpio_read,
    .write = mcxn_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void mcxn_gpio_reset(DeviceState *dev)
{
    MCXNGPIOState *s = MCXN_GPIO(dev);

    s->pdor = 0;
    s->pddr = 0;
    s->pidr = 0;
    s->lock = 0;
    s->in_level = 0;
}

static void mcxn_gpio_realize(DeviceState *dev, Error **errp)
{
    MCXNGPIOState *s = MCXN_GPIO(dev);

    memory_region_init_io(&s->iomem, OBJECT(s), &mcxn_gpio_ops, s,
                          TYPE_MCXN_GPIO, MCXN_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);

    qdev_init_gpio_in(dev, mcxn_gpio_set_input, MCXN_GPIO_PINS);
    qdev_init_gpio_out(dev, s->output, MCXN_GPIO_PINS);
}

static const VMStateDescription vmstate_mcxn_gpio = {
    .name = TYPE_MCXN_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pdor, MCXNGPIOState),
        VMSTATE_UINT32(pddr, MCXNGPIOState),
        VMSTATE_UINT32(pidr, MCXNGPIOState),
        VMSTATE_UINT32(lock, MCXNGPIOState),
        VMSTATE_UINT32(in_level, MCXNGPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static void mcxn_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mcxn_gpio_realize;
    device_class_set_legacy_reset(dc, mcxn_gpio_reset);
    dc->vmsd = &vmstate_mcxn_gpio;
}

static const TypeInfo mcxn_gpio_types[] = {
    {
        .name          = TYPE_MCXN_GPIO,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(MCXNGPIOState),
        .class_init    = mcxn_gpio_class_init,
    },
};

DEFINE_TYPES(mcxn_gpio_types)
