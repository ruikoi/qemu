/*
 * Milk-V DUO Board SPI Controller
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
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/multiarch/sg200x-spi.h"


#define FIFO_CAPACITY   8

static uint32_t spi_raw(SG200XSPIState *s)
{
    return s->errors | (s->tx_count <= s->regs[0x18 / 4]) |
           ((s->rx_count > s->regs[0x1c / 4]) << 4);
}

static void spi_update(SG200XSPIState *s)
{
    unsigned i;
    bool enabled = s->regs[2] & 1;

    for (i = 0; i < s->num_cs; i++) {
        bool high = !(enabled && (s->regs[4] & (1U << i)));
        DeviceState *slave = ssi_get_cs(s->spi, i);

        qemu_set_irq(s->cs_lines[i], high);
        if (slave &&
            SSI_PERIPHERAL_GET_CLASS(slave)->cs_polarity != SSI_CS_NONE) {
            qemu_set_irq(qdev_get_gpio_in_named(slave, SSI_GPIO_CS, 0), high);
        }
    }
    qemu_set_irq(s->irq, enabled && (spi_raw(s) & s->regs[0x2c / 4]));
}

static void spi_transfer(SG200XSPIState *s)
{
    unsigned bits = (s->regs[0] & 15) + 1;
    unsigned mode = (s->regs[0] >> 8) & 3;
    uint32_t mask = (1U << bits) - 1;

    if (s->regs[0] & 0x30) {
        qemu_log_mask(LOG_UNIMP, "sg200x: unsupported non-Motorola "
                      "frame mode\n");
        return;
    }
    if (!(s->regs[2] & 1) || !s->regs[4]) {
        return;
    }
    if (s->read_started && mode >= 2 && s->rx_remaining) {
        goto receive;
    }
    if (mode == 2 && !s->read_started) {
        s->read_started = true;
        s->rx_remaining = (s->regs[1] & 0xffff) + 1;
    }
    while (s->tx_count) {
        uint32_t tx = s->tx[0] & mask;
        uint32_t rx;

        memmove(s->tx, s->tx + 1, --s->tx_count * sizeof(s->tx[0]));
        rx = s->regs[0] & (1 << 11) ? tx : ssi_transfer(s->spi, tx);
        if (mode == 0) {
            if (s->rx_count == FIFO_CAPACITY) {
                s->errors |= 1 << 3;
            } else {
                s->rx[s->rx_count++] = rx & mask;
            }
        }
    }
    if (mode == 3 && !s->read_started) {
        s->read_started = true;
        s->rx_remaining = (s->regs[1] & 0xffff) + 1;
    }
receive:
    /* Functional model backpressures reads at FIFO depth, without DMA. */
    while (s->rx_remaining && s->rx_count < FIFO_CAPACITY) {
        s->rx[s->rx_count++] = ssi_transfer(s->spi, 0) & mask;
        s->rx_remaining--;
    }
    spi_update(s);
}

static void sg200x_spi_reset(DeviceState *d)
{
    SG200XSPIState *s = SG200X_SPI(d);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0] = 7;
    s->regs[0x2c / 4] = 0x3f;
    s->tx_count = s->rx_count = s->errors = s->rx_remaining = 0;
    s->read_started = false;
    spi_update(s);
}

static uint64_t sg200x_spi_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSPIState *s = opaque;
    uint32_t value = 0;

    if (addr >= 0x60 && addr < 0xf0) {
        if (!s->rx_count) {
            s->errors |= 1 << 2;
        } else {
            value = s->rx[0];
            memmove(s->rx, s->rx + 1, --s->rx_count * sizeof(s->rx[0]));
        }
    } else {
        switch (addr) {
        case 0x20: return s->tx_count;
        case 0x24: return s->rx_count;
        case 0x28:
            return (s->rx_remaining ? 1 : 0) | (s->tx_count < 8 ? 2 : 0) |
                   (!s->tx_count ? 4 : 0) |
                   (s->rx_count ? 8 : 0) | (s->rx_count == 8 ? 16 : 0);
        case 0x30: return spi_raw(s) & s->regs[0x2c / 4];
        case 0x34: return spi_raw(s);
        case 0x38:
            s->errors &= ~(1 << 1);
            break;
        case 0x3c:
            s->errors &= ~(1 << 3);
            break;
        case 0x40:
            s->errors &= ~(1 << 2);
            break;
        case 0x44:
            s->errors &= ~(1 << 5);
            break;
        case 0x48:
            s->errors = 0;
            break;
        default:
            if (addr < sizeof(s->regs)) {
                return s->regs[addr / 4];
            }
        }
    }
    spi_transfer(s);
    spi_update(s);
    return value;
}

static void sg200x_spi_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    SG200XSPIState *s = opaque;

    if (addr >= 0x60 && addr < 0xf0) {
        if (s->tx_count == FIFO_CAPACITY) {
            s->errors |= 1 << 1;
        } else {
            s->tx[s->tx_count++] = value;
        }
    } else if (addr < sizeof(s->regs)) {
        switch (addr) {
        case 0x00:
        case 0x04:
        case 0x0c:
        case 0x14:
            if (!(s->regs[2] & 1)) {
                s->regs[addr / 4] = value;
            }
            break;
        case 0x08:
            s->regs[2] = value & 1;
            if (!(value & 1)) {
                s->tx_count = s->rx_count = s->errors = 0;
                s->rx_remaining = 0;
                s->read_started = false;
            }
            break;
        case 0x10:
            if (!value) {
                s->read_started = false;
                s->rx_remaining = 0;
            }
            s->regs[4] = value & ((1U << s->num_cs) - 1);
            break;
        case 0x18:
        case 0x1c:
            s->regs[addr / 4] = value & 7;
            break;
        case 0x2c:
            s->regs[addr / 4] = value & 0x3f;
            break;
        case 0x4c:
        case 0x50:
        case 0x54:
        case 0xf0:
            s->regs[addr / 4] = value;
            break;
        default:
            break;
        }
    }
    spi_update(s);
    spi_transfer(s);
}

static const MemoryRegionOps sg200x_spi_ops = {
    .read = sg200x_spi_read,
    .write = sg200x_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void sg200x_spi_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SG200XSPIState *s = SG200X_SPI(dev);
    int i;

    if (!s->num_cs || s->num_cs > 4) {
        error_setg(errp, "SG200X SPI num-cs must be 1..4");
        return;
    }
    s->spi = ssi_create_bus(dev, s->bus_name ? s->bus_name : "spi");
    sysbus_init_irq(sbd, &s->irq);

    s->cs_lines = g_new0(qemu_irq, s->num_cs);
    for (i = 0; i < s->num_cs; i++) {
        sysbus_init_irq(sbd, &s->cs_lines[i]);
    }

    memory_region_init_io(&s->mmio, OBJECT(s), &sg200x_spi_ops, s,
                          TYPE_SG200X_SPI, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);


}

static const Property sg200x_spi_properties[] = {
    DEFINE_PROP_UINT32("num-cs", SG200XSPIState, num_cs, 1),
    DEFINE_PROP_STRING("bus-name", SG200XSPIState, bus_name),
};

static void sg200x_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, sg200x_spi_properties);
    device_class_set_legacy_reset(dc, sg200x_spi_reset);
    dc->realize = sg200x_spi_realize;
}

static const TypeInfo sg200x_spi_info = {
    .name = TYPE_SG200X_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SG200XSPIState),
    .class_init = sg200x_spi_class_init,
};

static void sg200x_spi_register_types(void)
{
    type_register_static(&sg200x_spi_info);
}

type_init(sg200x_spi_register_types)
