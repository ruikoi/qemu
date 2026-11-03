/*
 * Milk-V DUO Board General Purpose Input/Output Controller
 *
 * Copyright (c) 2025 Milk-V, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/multiarch/sg200x-gpio.h"
#include "migration/vmstate.h"

static void update_state(SG200XGPIOState *s)
{
    uint32_t active = s->int_en & ~s->ddr;
    uint32_t level = ~(s->in ^ s->int_polar);
    uint32_t changed = s->in ^ s->data;
    uint32_t edges = changed & level & s->int_level;
    unsigned i;

    s->int_raw_status = ((s->int_raw_status | edges) & s->int_level &
                         ~s->eoi) | (level & ~s->int_level);
    s->int_raw_status &= active;
    s->int_status = s->int_raw_status & ~s->int_mask;
    s->data = s->in;
    s->eoi = 0;
    for (i = 0; i < s->ngpio; i++) {
        qemu_set_irq(s->output[i], (s->dr & s->ddr) >> i & 1);
    }
    qemu_set_irq(s->irq, s->int_status != 0);
}

static uint64_t sg200x_gpio_read(void *opaque, hwaddr offset, unsigned int size)
{
    SG200XGPIOState *s = SG200X_GPIO(opaque);
    uint64_t r = 0;

    switch (offset) {
    case SG200X_GPIO_REG_SWPORTA_DR:
        r = s->dr;
        break;
    case SG200X_GPIO_REG_SWPORTA_DDR:
        r = s->ddr;
        break;
    case SG200X_GPIO_REG_INTEN:
        r = s->int_en;
        break;
    case SG200X_GPIO_REG_INTMASK:
        r = s->int_mask;
        break;
    case SG200X_GPIO_REG_INTTYPE_LEVEL:
        r = s->int_level;
        break;
    case SG200X_GPIO_REG_INT_POLARITY:
        r = s->int_polar;
        break;
    case SG200X_GPIO_REG_INTSTATUS:
        r = s->int_status;
        break;
    case SG200X_GPIO_REG_RAW_INTSTATUS:
        r = s->int_raw_status;
        break;
    case SG200X_GPIO_REG_DEBOUNCE:
        r = s->debounce;
        break;
    case SG200X_GPIO_REG_PORTA_EOI:
        break;
    case SG200X_GPIO_REG_EXT_PORTA:
        r = (s->data & ~s->ddr) | (s->dr & s->ddr);
        break;
    case SG200X_GPIO_REG_LS_SYNC:
        r = s->ls_sync;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
    return r;
}

static void sg200x_gpio_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned int size)
{
    SG200XGPIOState *s = SG200X_GPIO(opaque);
    switch (offset) {
    case SG200X_GPIO_REG_SWPORTA_DR:
        s->dr = value;
        break;
    case SG200X_GPIO_REG_SWPORTA_DDR:
        s->ddr = value;
        break;
    case SG200X_GPIO_REG_INTEN:
        s->int_en = value;
        break;
    case SG200X_GPIO_REG_INTMASK:
        s->int_mask = value;
        break;
    case SG200X_GPIO_REG_INTTYPE_LEVEL:
        s->int_level = value;
        break;
    case SG200X_GPIO_REG_INT_POLARITY:
        s->int_polar = value;
        break;
    case SG200X_GPIO_REG_DEBOUNCE:
        s->debounce = value;
        break;
    case SG200X_GPIO_REG_PORTA_EOI:
        s->eoi = value;
        break;
    case SG200X_GPIO_REG_LS_SYNC:
        s->ls_sync = value & 1;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
    update_state(s);
}

static const MemoryRegionOps gpio_ops = {
    .read = sg200x_gpio_read,
    .write = sg200x_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sg200x_gpio_set(void *opaque, int line, int value)
{
    SG200XGPIOState *s = SG200X_GPIO(opaque);

    assert(line >= 0 && line < SG200X_GPIO_PINS);

    if (value >= 0) {
        s->in = deposit32(s->in, line, 1, value != 0);
    }

    update_state(s);
}

static const VMStateDescription vmstate_sg200x_gpio = {
    .name = TYPE_SG200X_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]){
        VMSTATE_UINT32(value, SG200XGPIOState),
        VMSTATE_UINT32(dr, SG200XGPIOState),
        VMSTATE_UINT32(ddr, SG200XGPIOState),
        VMSTATE_UINT32(int_en, SG200XGPIOState),
        VMSTATE_UINT32(int_mask, SG200XGPIOState),
        VMSTATE_UINT32(int_level, SG200XGPIOState),
        VMSTATE_UINT32(int_polar, SG200XGPIOState),
        VMSTATE_UINT32(int_status, SG200XGPIOState),
        VMSTATE_UINT32(int_raw_status, SG200XGPIOState),
        VMSTATE_UINT32(debounce, SG200XGPIOState),
        VMSTATE_UINT32(eoi, SG200XGPIOState),
        VMSTATE_UINT32(ls_sync, SG200XGPIOState),
        VMSTATE_UINT32(in, SG200XGPIOState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sg200x_gpio_properties[] = {
    DEFINE_PROP_UINT32("ngpio", SG200XGPIOState, ngpio, SG200X_GPIO_PINS),
};

static void sg200x_gpio_realize(DeviceState *dev, Error **errp)
{
    SG200XGPIOState *s = SG200X_GPIO(dev);

    if (!s->ngpio || s->ngpio > SG200X_GPIO_PINS) {
        error_setg(errp, "SG200X GPIO ngpio must be 1..32");
        return;
    }
    memory_region_init_io(&s->mmio, OBJECT(dev), &gpio_ops, s,
                          TYPE_SG200X_GPIO, SG200X_GPIO_SIZE);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    qdev_init_gpio_in(DEVICE(s), sg200x_gpio_set, s->ngpio);
    qdev_init_gpio_out(DEVICE(s), s->output, s->ngpio);
}

static void sifive_gpio_reset(DeviceState *dev)
{
    SG200XGPIOState *s = SG200X_GPIO(dev);

    s->value = 0;
    s->dr = 0;
    s->ddr = 0;
    s->int_en = 0;
    s->int_mask = 0;
    s->int_level = 0;
    s->int_polar = 0;
    s->int_status = 0;
    s->int_raw_status = 0;
    s->debounce = 0;
    s->eoi = 0;
    s->ls_sync = 0;
    s->in = 0;
    s->data = 0;
    update_state(s);
}

static void sg200x_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sg200x_gpio_properties);
    dc->vmsd = &vmstate_sg200x_gpio;
    dc->realize = sg200x_gpio_realize;
    device_class_set_legacy_reset(dc, sifive_gpio_reset);
    dc->desc = "SG200X GPIO";
}

static const TypeInfo sg200x_gpio_info = {
    .name = TYPE_SG200X_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SG200XGPIOState),
    .class_init = sg200x_gpio_class_init,
};

static void sg200x_gpio_register_types(void)
{
    type_register_static(&sg200x_gpio_info);
}

type_init(sg200x_gpio_register_types)
