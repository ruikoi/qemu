/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental 25LC256-style 32 KiB SPI EEPROM; functional, untimed model. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/ssi/ssi.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"

#define TYPE_SG200X_SPI_EEPROM "sg200x-spi-eeprom"
OBJECT_DECLARE_SIMPLE_TYPE(SG200XEEPROMState, SG200X_SPI_EEPROM)

struct SG200XEEPROMState {
    SSIPeripheral parent_obj;
    uint8_t memory[32768], buffer[64];
    uint64_t dirty;
    uint16_t address, page_base;
    uint8_t command, status, phase, status_pending;
    char *image;
};

static bool protected_address(SG200XEEPROMState *s, unsigned address)
{
    unsigned bp = (s->status >> 2) & 3;

    return bp && address >= (bp == 1 ? 24576 : bp == 2 ? 16384 : 0);
}

static int eeprom_cs(SSIPeripheral *dev, bool high)
{
    SG200XEEPROMState *s = SG200X_SPI_EEPROM(dev);
    unsigned i;

    if (high) {
        if (s->command == 6 && s->phase == 1) {
            s->status |= 2;
        } else if (s->command == 4 && s->phase == 1) {
            s->status &= ~2;
        } else if ((s->status & 2) && s->command == 2 && s->dirty) {
            for (i = 0; i < 64; i++) {
                if ((s->dirty & (1ULL << i)) &&
                    !protected_address(s, s->page_base + i)) {
                    s->memory[s->page_base + i] = s->buffer[i];
                }
            }
            s->status &= ~2;
        } else if ((s->status & 2) && s->command == 1 && s->phase == 2) {
            s->status = s->status_pending & 0x8c;
        }
    }
    s->phase = 0;
    s->command = 0;
    s->dirty = 0;
    return 0;
}

static uint32_t eeprom_transfer(SSIPeripheral *dev, uint32_t value)
{
    SG200XEEPROMState *s = SG200X_SPI_EEPROM(dev);
    uint8_t result;

    if (!s->phase) {
        s->command = value;
        s->phase = 1;
        s->address = 0;
        return 0xff;
    }
    if (s->command == 5) {
        return s->status;
    }
    if (s->command == 1 && s->phase == 1) {
        s->status_pending = value;
        s->phase = 2;
        return 0xff;
    }
    if (s->command != 2 && s->command != 3) {
        return 0xff;
    }
    if (s->phase < 3) {
        s->address = ((s->address << 8) | (value & 0xff)) & 0x7fff;
        if (++s->phase == 3) {
            s->page_base = s->address & ~63;
        }
        return 0xff;
    }
    if (s->command == 3) {
        result = s->memory[s->address];
        s->address = (s->address + 1) & 0x7fff;
        return result;
    }
    if (s->status & 2) {
        s->buffer[s->address & 63] = value;
        s->dirty |= 1ULL << (s->address & 63);
    }
    s->address = s->page_base | ((s->address + 1) & 63);
    return 0xff;
}

static void eeprom_reset(DeviceState *dev)
{
    SG200XEEPROMState *s = SG200X_SPI_EEPROM(dev);

    /* Board reset does not erase nonvolatile bytes or block-protect bits. */
    s->status &= ~2;
    s->phase = s->command = 0;
    s->dirty = 0;
}

static void eeprom_realize(SSIPeripheral *dev, Error **errp)
{
    SG200XEEPROMState *s = SG200X_SPI_EEPROM(dev);
    g_autofree char *data = NULL;
    g_autoptr(GError) error = NULL;
    gsize length;

    memset(s->memory, 0xff, sizeof(s->memory));
    if (s->image) {
        if (!g_file_get_contents(s->image, &data, &length, &error)) {
            error_setg(errp, "cannot load EEPROM image: %s", error->message);
            return;
        }
        if (length > sizeof(s->memory)) {
            error_setg(errp, "EEPROM image must be at most 32768 bytes");
            return;
        }
        memcpy(s->memory, data, length);
    }
}

static const Property eeprom_props[] = {
    DEFINE_PROP_STRING("image", SG200XEEPROMState, image),
};

static void eeprom_class_init(ObjectClass *oc, const void *data)
{
    SSIPeripheralClass *sc = SSI_PERIPHERAL_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    sc->realize = eeprom_realize;
    sc->transfer = eeprom_transfer;
    sc->set_cs = eeprom_cs;
    sc->cs_polarity = SSI_CS_LOW;
    device_class_set_props(dc, eeprom_props);
    device_class_set_legacy_reset(dc, eeprom_reset);
}

static const TypeInfo eeprom_info = {
    .name = TYPE_SG200X_SPI_EEPROM,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(SG200XEEPROMState),
    .class_init = eeprom_class_init,
};

static void eeprom_register(void)
{
    type_register_static(&eeprom_info);
}
type_init(eeprom_register)
