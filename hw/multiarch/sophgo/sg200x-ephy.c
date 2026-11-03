/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local experimental SDK EPHY APB initialization, without Ethernet traffic. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"
#include "qemu/module.h"
#include "qemu/log.h"

#define TYPE_SG200X_EPHY "sg200x.ephy"
OBJECT_DECLARE_SIMPLE_TYPE(SG200XEPHYState, SG200X_EPHY)

struct SG200XEPHYState {
    SysBusDevice parent_obj;
    MemoryRegion mmio, gmac_mmio, calibration_mmio;
    uint32_t gmac[0x1100 / 4];
    uint16_t regs[256][32];
    uint16_t control, access;
    uint8_t page;
};

static uint64_t ephy_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XEPHYState *s = opaque;

    if (addr == 0x800) {
        return s->control;
    }
    if (addr == 0x804) {
        return s->access;
    }
    if (addr == 0x7c) {
        return s->page << 8;
    }
    if (addr < 0x7c) {
        /* Analog training is assumed complete, as in preinitialized FIP. */
        if (s->page == 5 && addr == 0x48) {
            return s->regs[5][0x12] | 0x8000;
        }
        return s->regs[s->page][addr / 4];
    }
    qemu_log_mask(LOG_UNIMP, "sg200x: EPHY APB read 0x%" HWADDR_PRIx "\n",
                  addr);
    return 0;
}

static void ephy_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    SG200XEPHYState *s = opaque;

    if (addr == 0x800) {
        s->control = value;
    } else if (addr == 0x804) {
        s->access = value & 1;
    } else if (addr == 0x7c) {
        s->page = value >> 8;
    } else if (addr < 0x7c) {
        s->regs[s->page][addr / 4] = value;
    } else {
        qemu_log_mask(LOG_UNIMP, "sg200x: EPHY APB write 0x%" HWADDR_PRIx
                      "\n", addr);
    }
}

static const MemoryRegionOps ephy_ops = {
    .read = ephy_read, .write = ephy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* SG200X TRM GMAC MDIO operation; management only, no packet DMA. */
static uint64_t gmac_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XEPHYState *s = opaque;

    return s->gmac[addr / 4];
}

static void gmac_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    SG200XEPHYState *s = opaque;

    if (addr == 0x10 && (value & 1)) {
        unsigned phy = (value >> 11) & 31;
        unsigned reg = (value >> 6) & 31;

        /* Internal CVITEK PHY is address zero; other addresses are absent. */
        if (phy == 0) {
            if (value & 2) {
                uint16_t data = s->gmac[0x14 / 4];

                if (reg == 31) {
                    s->page = data >> 8;
                } else if (!(s->page == 0 && reg >= 1 && reg <= 3)) {
                    if (reg == 0) {
                        data &= ~0x8000; /* BMCR reset completes immediately. */
                    }
                    s->regs[s->page][reg] = data;
                }
            } else {
                s->gmac[0x14 / 4] = reg == 31 ? s->page << 8 :
                                              s->regs[s->page][reg];
            }
        } else if (!(value & 2)) {
            s->gmac[0x14 / 4] = 0xffff;
        }
        value &= ~1; /* MDIO operation completes synchronously. */
    } else if (addr == 0x1000 && (value & 1)) {
        memset(s->gmac, 0, sizeof(s->gmac));
        s->gmac[0x20 / 4] = 0x1037; /* Synopsys GMAC 3.7 */
        value &= ~1;
    } else if (addr == 0x1014 || addr == 0x38) {
        s->gmac[addr / 4] &= ~value;
        return;
    } else if (addr == 0x20 || addr == 0x1058) {
        return;
    }
    s->gmac[addr / 4] = value;
}

static const MemoryRegionOps gmac_ops = {
    .read = gmac_read, .write = gmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Unprogrammed EPHY calibration fuses select SDK fallback coefficients. */
static uint64_t calibration_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void calibration_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "sg200x: calibration fuse programming unsupported\n");
}

static const MemoryRegionOps calibration_ops = {
    .read = calibration_read, .write = calibration_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void ephy_reset(DeviceState *dev)
{
    SG200XEPHYState *s = SG200X_EPHY(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->page = s->control = s->access = 0;
    s->regs[0][0] = 0x1000; /* Autonegotiation enabled. */
    s->regs[0][1] = 0x7809; /* 10/100 capabilities, link down. */
    s->regs[0][2] = 0x0043;
    s->regs[0][3] = 0x5649;
    memset(s->gmac, 0, sizeof(s->gmac));
    s->gmac[0x20 / 4] = 0x1037;
}

static void ephy_init(Object *obj)
{
    SG200XEPHYState *s = SG200X_EPHY(obj);

    memory_region_init_io(&s->mmio, obj, &ephy_ops, s, TYPE_SG200X_EPHY,
                          0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    memory_region_init_io(&s->gmac_mmio, obj, &gmac_ops, s,
                          "sg200x.gmac-init", sizeof(s->gmac));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->gmac_mmio);
    memory_region_init_io(&s->calibration_mmio, obj, &calibration_ops, s,
                          "sg200x.ephy-calibration", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->calibration_mmio);
}

static void ephy_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), ephy_reset);
}

static const TypeInfo ephy_info = {
    .name = TYPE_SG200X_EPHY,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SG200XEPHYState),
    .instance_init = ephy_init,
    .class_init = ephy_class_init,
};

static void ephy_register(void)
{
    type_register_static(&ephy_info);
}
type_init(ephy_register)
