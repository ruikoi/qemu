/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local experimental SG200X hardware subset; not an upstream submission. */
#include "qemu/osdep.h"
#include "elf.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/multiarch/sg200x.h"
#include "hw/char/serial-mm.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "system/rtc.h"
#include "system/runstate.h"
#include "system/cpus.h"
#include "system/system.h"
#include "migration/blocker.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/sd.h"
#include "hw/i2c/designware_i2c.h"
#include "hw/usb/hcd-dwc2.h"
#include "system/blockdev.h"

/* TRM AP_SYS_CTRL at 0x01901000; SDK cpu_pwr_ctrl selects isolation. */
static uint64_t ap_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr == 4 || addr == 8) {
        return s->ap_pwr_iso[addr / 4 - 1];
    }
    qemu_log_mask(LOG_UNIMP, "sg200x: AP_SYS_CTRL read 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void ap_ctrl_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr == 4 || addr == 8) {
        /* Inactive alternate ISA has no instantiated CPU in this model. */
        s->ap_pwr_iso[addr / 4 - 1] = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sg200x: AP_SYS_CTRL write 0x%" HWADDR_PRIx
                  "\n", addr);
}

static const MemoryRegionOps ap_ctrl_ops = {
    .read = ap_ctrl_read, .write = ap_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Digital mux configuration only: no analog pad or clock side effects. */
static uint64_t pinmux_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;

    return (regs[addr / 4] >> ((addr & 3) * 8)) &
           ((1ULL << (size * 8)) - 1);
}

static void pinmux_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    uint32_t *regs = opaque;

    uint32_t shift = (addr & 3) * 8;
    uint32_t mask = ((1ULL << (size * 8)) - 1) << shift;

    regs[addr / 4] = (regs[addr / 4] & ~mask) | ((value << shift) & mask);
}

static const MemoryRegionOps pinmux_ops = {
    .read = pinmux_read, .write = pinmux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* AXI DMAC initialization subset: reset and idle channels, no transfers. */
static uint64_t dma_cfg_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr == 8) {
        return 0x3130312a; /* DW AXI DMA component version */
    }
    return s->dma_cfg[addr / 4];
}

static void dma_cfg_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr == 0x58 && (value & 1)) {
        memset(s->dma_cfg, 0, sizeof(s->dma_cfg));
    } else if (addr == 0x18 || addr == 0x1c) {
        if (value & 0xff) {
            qemu_log_mask(LOG_UNIMP, "sg200x: AXI DMA transfers unsupported\n");
        }
        /* Channels stay idle; no fabricated completion or memory writes. */
    } else if (addr == 0 || addr == 4 || addr == 8 || addr == 0xc ||
               addr == 0x30 || addr == 0x50 || addr == 0x58) {
        return;
    } else if (addr >= 0x100 && (addr & 0xff) == 0x98) {
        s->dma_cfg[(addr - 0x10) / 4] &= ~value;
    } else {
        s->dma_cfg[addr / 4] = value;
    }
}

static const MemoryRegionOps dma_cfg_ops = {
    .read = dma_cfg_read, .write = dma_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* Fixed ambient sample: SDK conversion of 852 is approximately 25 C. */
static uint64_t thermal_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr == 0x20 || addr == 0x24) {
        return 852 | (852 << 16);
    }
    if (addr == 8 || addr == 0x18 || addr == 0x1c) {
        return 0; /* No conversion busy state or overheat events. */
    }
    return s->thermal[addr / 4];
}

static void thermal_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr != 0x14 && addr != 0x18 && addr != 0x1c &&
        addr != 0x20 && addr != 0x24) {
        s->thermal[addr / 4] = value;
    }
}

static const MemoryRegionOps thermal_ops = {
    .read = thermal_read, .write = thermal_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void watchdog_reload(SG200XSoCState *s, bool initial)
{
    unsigned top = (s->watchdog_range >> (initial ? 4 : 0)) & 15;
    uint64_t delay = (1ULL << (16 + top)) * NANOSECONDS_PER_SECOND /
                     SG200X_TIME_HZ;

    timer_mod(s->watchdog_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static void watchdog_expire(void *opaque)
{
    SG200XSoCState *s = opaque;

    if ((s->watchdog_control & 2) && !s->watchdog_expired) {
        s->watchdog_expired = true;
        s->watchdog_status = 1;
        qemu_set_irq(s->watchdog_irq, 1);
        watchdog_reload(s, false);
    } else {
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    }
}

static uint64_t watchdog_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    switch (addr) {
    case 0: return s->watchdog_control;
    case 4: return s->watchdog_range;
    case 8:
        if (timer_pending(s->watchdog_timer)) {
            int64_t left = timer_expire_time_ns(s->watchdog_timer) -
                           qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            return left > 0 ? muldiv64(left, SG200X_TIME_HZ,
                                      NANOSECONDS_PER_SECOND) : 0;
        }
        return 0;
    case 0x10: return s->watchdog_status;
    case 0x14:
        s->watchdog_status = 0;
        qemu_set_irq(s->watchdog_irq, 0);
        return 0;
    case 0xf4: return 1 << 6; /* Fixed TOP: 2^(16+n) cycles. */
    case 0xf8: return 0x3130312a;
    case 0xfc: return 0x44570120;
    default: return 0;
    }
}

static void watchdog_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr == 0) {
        bool start = !(s->watchdog_control & 1) && (value & 1);

        s->watchdog_control = (value & 0x1f) | (s->watchdog_control & 1);
        if (start) {
            watchdog_reload(s, true);
        }
    } else if (addr == 4) {
        s->watchdog_range = value & 0xff;
    } else if (addr == 0xc && (value & 0xff) == 0x76 &&
               (s->watchdog_control & 1)) {
        s->watchdog_status = 0;
        s->watchdog_expired = false;
        qemu_set_irq(s->watchdog_irq, 0);
        watchdog_reload(s, false);
    }
}

static const MemoryRegionOps watchdog_ops = {
    .read = watchdog_read, .write = watchdog_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* RTC seconds progress with virtual time; warm reset retains battery time. */
static uint32_t rtc_seconds(SG200XSoCState *s)
{
    return s->rtc_seconds +
        (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->rtc_epoch_ns) /
        NANOSECONDS_PER_SECOND;
}

static uint64_t rtc_core_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    switch (addr) {
    case 0x18:
    case 0x4a8: return rtc_seconds(s);
    case 0xd0: return s->rtc_vbat;
    case 0xd4: return 3; /* RTC_ST, ST_ON */
    default: return s->rtc_core[addr / 4];
    }
}

static void rtc_core_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    SG200XSoCState *s = opaque;

    switch (addr) {
    case 0x18:
    case 0x4a8:
    case 0xd4:
        return;
    case 0xd0:
        s->rtc_vbat = value & 7;
        return;
    case 0x14:
        if (value & 1) {
            s->rtc_seconds = s->rtc_core[0x10 / 4];
            s->rtc_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        return; /* Seconds load trigger completes synchronously. */
    default:
        s->rtc_core[addr / 4] = value;
        if (addr == 0xc && (value & 1)) {
            qemu_log_mask(LOG_UNIMP, "sg200x: RTC alarm IRQ not implemented\n");
        }
    }
}

static const MemoryRegionOps rtc_core_ops = {
    .read = rtc_core_read, .write = rtc_core_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* SDHCI 0x200/204 and PHY 0x240..24c, alongside standard SDHCI. */
static uint64_t sd_vendor_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t *regs = opaque;

    if (addr == 0 || addr == 4 || (addr >= 0x40 && addr <= 0x4c)) {
        return regs[addr / 4];
    }
    qemu_log_mask(LOG_UNIMP, "sg200x: SD vendor read 0x%" HWADDR_PRIx
                  "\n", addr);
    return 0;
}

static void sd_vendor_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    uint32_t *regs = opaque;

    if (addr == 0 || addr == 4 || addr == 0x40 || addr == 0x44 ||
        addr == 0x4c) {
        regs[addr / 4] = value;
    } else {
        qemu_log_mask(LOG_UNIMP, "sg200x: SD vendor write 0x%" HWADDR_PRIx
                      "\n", addr);
    }
}

static const MemoryRegionOps sd_vendor_ops = {
    .read = sd_vendor_read, .write = sd_vendor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

typedef struct SmallResetRequest {
    bool run;
    uint64_t entry;
} SmallResetRequest;

static void small_reset_on_cpu(CPUState *cpu, run_on_cpu_data data)
{
    SmallResetRequest *request = data.host_ptr;

    cpu_reset(cpu);
    if (request->run) {
        cpu_set_pc(cpu, request->entry);
    }
    cpu->halted = !request->run;
}

static void small_reset(SG200XSoCState *s, bool run, uint64_t entry)
{
    SmallResetRequest request = { .run = run, .entry = entry };

    if (!s->small_cpu) {
        return;
    }
    /* Reset registers may be written while the peer vCPU is executing. */
    run_on_cpu(s->small_cpu, small_reset_on_cpu,
               RUN_ON_CPU_HOST_PTR(&request));
    qemu_cpu_kick(s->small_cpu);
}

static uint64_t reset_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr == 0x24) {
        return s->resetn;
    }
    qemu_log_mask(LOG_UNIMP, "sg200x: reset register read 0x%" HWADDR_PRIx "\n",
                   addr);
    return 0;
}

static void reset_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    SG200XSoCState *s = opaque;
    bool old;

    if (addr != 0x24) {
        qemu_log_mask(LOG_UNIMP, "sg200x: reset register write 0x%" HWADDR_PRIx
                       "\n", addr);
        return;
    }
    old = s->resetn & (1 << 6);
    s->resetn = value;
    if (old && !(value & (1 << 6))) {
        small_reset(s, false, 0);
    } else if (!old && (value & (1 << 6))) {
        if (!(s->secure_ctrl & (1 << 13))) {
            qemu_log_mask(LOG_GUEST_ERROR,
                           "sg200x: small core release without boot override\n");
            return;
        }
        small_reset(s, true, ((uint64_t)s->entry_hi << 32) | s->entry_lo);
    }
}

static uint64_t secure_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    switch (addr) {
    case 0x04: return s->secure_ctrl;
    case 0x20: return s->entry_lo;
    case 0x24: return s->entry_hi;
    default:
        qemu_log_mask(LOG_UNIMP, "sg200x: SEC_SYS read 0x%" HWADDR_PRIx "\n",
                       addr);
        return 0;
    }
}

static void secure_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    SG200XSoCState *s = opaque;

    switch (addr) {
    case 0x04:
        s->secure_ctrl = value;
        break;
    case 0x20:
        s->entry_lo = value;
        break;
    case 0x24:
        s->entry_hi = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sg200x: SEC_SYS write 0x%" HWADDR_PRIx "\n",
                       addr);
    }
}

static const MemoryRegionOps reset_ops = {
    .read = reset_read, .write = reset_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps secure_ops = {
    .read = secure_read, .write = secure_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t uart_aux_read(void *opaque, hwaddr addr, unsigned size)
{
    SerialMM *uart = opaque;

    switch (addr) {
    case 0x7c: /* DW_UART_USR: busy, TFNF, TFE, RFNE, RFF */
        return ((uart->serial.lsr & 0x40) ? 6 : 3) |
               ((uart->serial.lsr & 1) ? 8 : 0);
    case 0xf4: return 0; /* No optional DW extensions advertised. */
    case 0xf8: return 0x3430302a; /* DW component version */
    case 0xfc: return 0x44570110; /* DW component type */
    default:
        qemu_log_mask(LOG_UNIMP, "sg200x: DW UART read 0x%" HWADDR_PRIx "\n",
                       addr);
        return 0;
    }
}

static void uart_aux_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "sg200x: DW UART write 0x%" HWADDR_PRIx "\n",
                   addr);
}

static const MemoryRegionOps uart_aux_ops = {
    .read = uart_aux_read, .write = uart_aux_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void uart_create(SG200XSoCState *s, unsigned index, qemu_irq irq)
{
    hwaddr addr = index == 4 ? 0x041c0000 : SG200X_UART0 + index * 0x10000;
    SerialMM *uart = serial_mm_init(get_system_memory(), addr, 2, irq,
                                    SG200X_TIME_HZ / 16, serial_hd(index),
                                    DEVICE_LITTLE_ENDIAN);

    memory_region_init_io(&s->uart_aux[index], OBJECT(s), &uart_aux_ops, uart,
                           "sg200x.dw-uart-extra", 0x10000);
    memory_region_add_subregion_overlap(get_system_memory(), addr,
                                        &s->uart_aux[index], -1);
}

/* Only the documented general-purpose TOP scratch registers are modeled. */
static uint64_t top_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr >= 0x80 && addr <= 0x8c && !(addr & 3)) {
        return s->top_gp[(addr - 0x80) / 4];
    }
    qemu_log_mask(LOG_UNIMP, "sg200x: TOP read 0x%" HWADDR_PRIx "\n", addr);
    return 0;
}

static void top_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    SG200XSoCState *s = opaque;

    if (addr >= 0x80 && addr <= 0x8c && !(addr & 3)) {
        s->top_gp[(addr - 0x80) / 4] = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "sg200x: TOP write 0x%" HWADDR_PRIx "\n", addr);
}

static const MemoryRegionOps top_ops = {
    .read = top_read, .write = top_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void peripherals_create(SG200XSoCState *s)
{
    DeviceState *dev = qdev_new("sg200x.ephy");
    unsigned i;

    object_property_add_child(OBJECT(s), "ephy", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x03009000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0x04070000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, 0x03050120);
    for (i = 0; i < 5; i++) {
        g_autofree char *name = g_strdup_printf("i2c%u", i);

        dev = qdev_new(TYPE_DESIGNWARE_I2C);
        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x04000000 + i * 0x10000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
            qdev_get_gpio_in(s->main_intc, (s->arm ? 33 : 49) + i));
    }
    dev = qdev_new(TYPE_DWC2_USB);
    object_property_add_const_link(OBJECT(dev), "dma-mr",
                                   OBJECT(get_system_memory()));
    object_property_add_child(OBJECT(s), "usb", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x04340000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                      qdev_get_gpio_in(s->main_intc, s->arm ? 14 : 30));
    dev = qdev_new("sg200x.timer");
    object_property_add_child(OBJECT(s), "timer", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x030a0000);
    for (i = 0; i < 8; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
            qdev_get_gpio_in(s->main_intc, (s->arm ? 63 : 79) + i));
    }
    for (i = 0; i < 5; i++) {
        g_autofree char *name = g_strdup_printf("gpio%u", i);

        dev = qdev_new("sg200x.gpio");
        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0,
                       i == 4 ? 0x05021000 : 0x03020000 + i * 0x1000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
            qdev_get_gpio_in(s->main_intc, i == 4 ? (s->arm ? 54 : 70) :
                            (s->arm ? 44 : 60) + i));
    }
    for (i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("spi%u", i);

        g_autofree char *bus_name = g_strdup_printf("sg200x-spi%u", i);

        dev = qdev_new("sg200x.spi");
        qdev_prop_set_string(dev, "bus-name", bus_name);
        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x04180000 + i * 0x10000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
            qdev_get_gpio_in(s->main_intc, (s->arm ? 38 : 54) + i));
    }
}

static void sd_create(SG200XSoCState *s)
{
    unsigned i;

    /* SD0 has the image; Wi-Fi SDIO and eMMC are enumerated without cards. */
    for (i = 0; i < 3; i++) {
        DeviceState *host = qdev_new(TYPE_SYSBUS_SDHCI);
        SysBusDevice *bus = SYS_BUS_DEVICE(host);
        DriveInfo *drive = i ? NULL : drive_get(IF_SD, 0, 0);
        hwaddr base = i == 2 ? 0x04300000 : 0x04310000 + i * 0x10000;

        qdev_prop_set_uint8(host, "sd-spec-version", 3);
        qdev_prop_set_bit(host, "wp-inverted", true);
        sysbus_realize_and_unref(bus, &error_fatal);
        sysbus_mmio_map(bus, 0, base);
        memory_region_init_io(&s->sd_vendor_mmio[i], OBJECT(s), &sd_vendor_ops,
                              s->sd_vendor[i], "sg200x.sd-phy", 0x100);
        memory_region_add_subregion(get_system_memory(), base + 0x200,
                                    &s->sd_vendor_mmio[i]);
        sysbus_connect_irq(bus, 0,
                          qdev_get_gpio_in(s->main_intc,
                                           i == 2 ? (s->arm ? 18 : 34) :
                                           (s->arm ? 20 : 36) + i * 2));
        if (drive) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(drive),
                                   &error_fatal);
            qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"),
                                   &error_fatal);
        }
    }
}

static void soc_reset(void *opaque)
{
    SG200XSoCState *s = opaque;

    memset(s->top_gp, 0, sizeof(s->top_gp));
    s->rtc_vbat = 7;
    memset(s->rtc_core, 0, sizeof(s->rtc_core));
    memset(s->rtc_ctrl, 0, sizeof(s->rtc_ctrl));
    s->rtc_ctrl[8 / 4] = 1 << 10; /* External 32 kHz crystal, no calibration. */
    s->watchdog_control = s->watchdog_range = s->watchdog_status = 0;
    s->watchdog_expired = false;
    timer_del(s->watchdog_timer);
    qemu_set_irq(s->watchdog_irq, 0);
    memset(s->audio_cfg, 0, sizeof(s->audio_cfg));
    memset(s->i2s_cfg, 0, sizeof(s->i2s_cfg));
    memset(s->codec_cfg, 0, sizeof(s->codec_cfg));
    memset(s->usb_phy, 0, sizeof(s->usb_phy));
    memset(s->thermal, 0, sizeof(s->thermal));
    memset(s->dma_cfg, 0, sizeof(s->dma_cfg));
    memset(s->sd_vendor, 0, sizeof(s->sd_vendor));
    s->sd_vendor[0][0x4c / 4] = 1;
    s->sd_vendor[1][0x4c / 4] = 1;
    s->sd_vendor[2][0x4c / 4] = 1;
    memset(s->ap_pwr_iso, 0, sizeof(s->ap_pwr_iso));
    memset(s->pinmux, 0, sizeof(s->pinmux));
    memset(s->pad, 0, sizeof(s->pad));
    memset(s->rtc_pinmux, 0, sizeof(s->rtc_pinmux));
    if (s->rom_boot) {
        cpu_reset(s->main_cpu);
    }
    s->resetn = s->rtos_loaded ? UINT32_MAX : UINT32_MAX & ~(1 << 6);
    s->secure_ctrl = s->rtos_loaded ? 1 << 13 : 0;
    s->entry_lo = s->rtos_entry;
    s->entry_hi = s->rtos_entry >> 32;
    small_reset(s, s->rtos_loaded, s->rtos_entry);
}

static void soc_realize(DeviceState *dev, Error **errp)
{
    SG200XSoCState *s = SG200X_SOC(dev);
    MultiarchDomainConfig config = {
        .id = 0, .resetvec = s->rom_boot ? s->rom_base : SG200X_ROM,
    };
    const MultiarchCoreProfile *profile = multiarch_core_profile_find(
        s->arm ? "sg200x-a53" : "sg200x-c906");

    s->main_domain = object_new(TYPE_CONTAINER);
    object_property_add_child(OBJECT(s), "main-domain", s->main_domain);
    object_unref(s->main_domain);
    s->main_cpu = multiarch_domain_create(s->main_domain, get_system_memory(),
                                          profile, &config);
    if (s->arm) {
        sg200x_arm_intc(s);
    } else {
        sg200x_riscv_intc(s, false);
    }
    uart_create(s, 0, qdev_get_gpio_in(s->main_intc, s->arm ? 28 : 44));
    for (unsigned i = 2; i < 5; i++) {
        uart_create(s, i, qdev_get_gpio_in(s->main_intc,
                                         (s->arm ? 28 : 44) + i));
    }
    memory_region_init_ram_flags_nomigrate(&s->tpu_sram, OBJECT(s),
                                           "sg200x.tpu-sram", s->tpu_size, 0,
                                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x0c000000, &s->tpu_sram);
    memory_region_init_alias(&s->tpu_shadow, OBJECT(s), "sg200x.tpu-shadow",
                             &s->tpu_sram, 0, s->tpu_size);
    memory_region_add_subregion(
        get_system_memory(),
        s->tpu_size == 0x10000 ? 0x3c000000 : 0x2c000000, &s->tpu_shadow);
    memory_region_init_rom(&s->rom, OBJECT(s), "sg200x.boot-rom",
                           s->rom_base - SG200X_ROM + s->rom_size,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), SG200X_ROM, &s->rom);
    memory_region_init_ram_flags_nomigrate(&s->rtc_sram, OBJECT(s),
                                           "sg200x.rtc-sram", 0x6000, 0,
                                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x05200000, &s->rtc_sram);
    /* SDK boot metadata window; not a claim about all AXI SRAM aliases. */
    memory_region_init_ram_flags_nomigrate(&s->axi_sram, OBJECT(s),
                                           "sg200x.axi-boot-scratch", 0x40, 0,
                                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x0e000000, &s->axi_sram);
    memory_region_init_io(&s->top_mmio, OBJECT(s), &top_ops, s,
                         "sg200x.top", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x03000000, &s->top_mmio);
    memory_region_init_io(&s->ap_ctrl_mmio, OBJECT(s), &ap_ctrl_ops, s,
                          "sg200x.ap-sys-ctrl", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x01901000,
                               &s->ap_ctrl_mmio);
    memory_region_init_io(&s->pinmux_mmio, OBJECT(s), &pinmux_ops, s->pinmux,
                          "sg200x.pinmux", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x03001000,
                               &s->pinmux_mmio);
    memory_region_init_io(&s->rtc_pinmux_mmio, OBJECT(s), &pinmux_ops,
                          s->rtc_pinmux, "sg200x.rtc-pinmux", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x05027000,
                               &s->rtc_pinmux_mmio);
    /* I2S subsystem routing/clock configuration, no audio data path. */
    memory_region_init_io(&s->usb_phy_mmio, OBJECT(s), &pinmux_ops,
                          s->usb_phy, "sg200x.usb-phy-config", 0x80);
    memory_region_add_subregion(get_system_memory(), 0x03006000,
                               &s->usb_phy_mmio);
    s->watchdog_irq = qdev_get_gpio_in(s->main_intc, s->arm ? 42 : 58);
    s->watchdog_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, watchdog_expire, s);
    memory_region_init_io(&s->watchdog_mmio, OBJECT(s), &watchdog_ops, s,
                          "sg200x.watchdog", 0x100);
    memory_region_add_subregion(get_system_memory(), 0x03010000,
                               &s->watchdog_mmio);
    memory_region_init_io(&s->thermal_mmio, OBJECT(s), &thermal_ops, s,
                          "sg200x.temperature", 0x100);
    memory_region_add_subregion(get_system_memory(), 0x030e0000,
                               &s->thermal_mmio);
    /* Audio control registers only; no PCM FIFO, DMA or sound backend. */
    for (unsigned i = 0; i < 4; i++) {
        memory_region_init_io(&s->i2s_mmio[i], OBJECT(s), &pinmux_ops,
                              s->i2s_cfg[i], "sg200x.i2s-config", 0x100);
        memory_region_add_subregion(get_system_memory(),
                                   0x04100000 + i * 0x10000, &s->i2s_mmio[i]);
    }
    memory_region_init_io(&s->codec_mmio, OBJECT(s), &pinmux_ops,
                          s->codec_cfg, "sg200x.audio-codec-config", 0x200);
    memory_region_add_subregion(get_system_memory(), 0x0300a000,
                               &s->codec_mmio);
    memory_region_init_io(&s->audio_cfg_mmio, OBJECT(s), &pinmux_ops,
                          s->audio_cfg, "sg200x.audio-config", 0x100);
    memory_region_add_subregion(get_system_memory(), 0x04108000,
                               &s->audio_cfg_mmio);
    memory_region_init_io(&s->dma_cfg_mmio, OBJECT(s), &dma_cfg_ops, s,
                          "sg200x.axi-dma-init", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x04330000,
                               &s->dma_cfg_mmio);
    memory_region_init_io(&s->rtc_core_mmio, OBJECT(s), &rtc_core_ops, s,
                          "sg200x.rtc-core", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x05026000,
                               &s->rtc_core_mmio);
    {
        struct tm tm;

        qemu_get_timedate(&tm, 0);
        s->rtc_seconds = mktimegm(&tm);
        s->rtc_epoch_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    memory_region_init_io(&s->rtc_ctrl_mmio, OBJECT(s), &pinmux_ops,
                          s->rtc_ctrl, "sg200x.rtc-control", 0x100);
    memory_region_add_subregion(get_system_memory(), 0x05025000,
                               &s->rtc_ctrl_mmio);
    memory_region_init_io(&s->pad_mmio, OBJECT(s), &pinmux_ops, s->pad,
                          "sg200x.pad-config", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x03002000, &s->pad_mmio);
    peripherals_create(s);
    sd_create(s);
    memory_region_init_io(&s->reset_mmio, OBJECT(s), &reset_ops, s,
                           "sg200x.reset", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x03003000,
                                 &s->reset_mmio);
    memory_region_init_io(&s->secure_mmio, OBJECT(s), &secure_ops, s,
                           "sg200x.sec-sys", 0x1000);
    memory_region_add_subregion(get_system_memory(), 0x020b0000,
                                 &s->secure_mmio);
    error_setg(&s->migration_blocker,
                "SG200X experimental SoC has no migration");
    migrate_add_blocker(&s->migration_blocker, &error_fatal);
}

void sg200x_soc_attach_small(SG200XSoCState *s)
{
    const MultiarchDomainConfig config = { .id = 1, .resetvec = SG200X_ROM };
    const MultiarchCoreProfile *profile =
        multiarch_core_profile_find("sg200x-c906");

    memory_region_init(&s->small_root, OBJECT(s), "sg200x.small-memory",
                        UINT64_MAX);
    memory_region_init_alias(&s->shared_view, OBJECT(s), "sg200x.shared-view",
                             get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion_overlap(&s->small_root, 0, &s->shared_view, -1);
    s->small_domain = object_new(TYPE_CONTAINER);
    object_property_add_child(OBJECT(s), "small-domain", s->small_domain);
    object_unref(s->small_domain);
    s->small_cpu = multiarch_domain_create(s->small_domain, &s->small_root,
                                           profile, &config);
    sg200x_riscv_intc(s, true);
    uart_create(s, 1, qdev_get_gpio_in(s->small_intc, 31));
    if (s->rtos) {
        uint64_t low, high;
        bool is64;
        Elf64_Ehdr header;

        if (!load_elf_hdr(s->rtos, &header, &is64, &error_fatal) ||
            !is64 || header.e_ident[EI_DATA] != ELFDATA2LSB) {
            error_report("rtos must be a little-endian RV64 ELF");
            exit(EXIT_FAILURE);
        }
        ssize_t loaded = load_elf_as(s->rtos, NULL, NULL, NULL, &s->rtos_entry,
                                     &low, &high, NULL, 0, EM_RISCV, 1, 0,
                                     NULL);
        if (loaded <= 0 || high <= low ||
            !((low >= SG200X_DDR + s->ram_size - SG200X_RTOS_SIZE &&
               high <= SG200X_DDR + s->ram_size) ||
              (low >= 0x0c000000 && high <= 0x0c000000 + s->tpu_size)) ||
            s->rtos_entry < low || s->rtos_entry >= high) {
            error_report("rtos must be a RISC-V ELF in TPU SRAM or reserved "
                         "top 8 MiB of DDR");
            exit(EXIT_FAILURE);
        }
        s->rtos_loaded = true;
    }
    qemu_register_reset(soc_reset, s);
}

static const Property soc_properties[] = {
    DEFINE_PROP_BOOL("rom-boot", SG200XSoCState, rom_boot, false),
    DEFINE_PROP_UINT64("rom-base", SG200XSoCState, rom_base, SG200X_ROM),
    DEFINE_PROP_UINT64("rom-size", SG200XSoCState, rom_size, 0x10000),
    DEFINE_PROP_UINT64("tpu-size", SG200XSoCState, tpu_size, 0x10000),
    DEFINE_PROP_BOOL("arm-main", SG200XSoCState, arm, false),
    DEFINE_PROP_UINT64("ram-size", SG200XSoCState, ram_size, 0),
    DEFINE_PROP_STRING("rtos", SG200XSoCState, rtos),
};

static void soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = soc_realize;
    dc->user_creatable = false;
    device_class_set_props(dc, soc_properties);
}

static const TypeInfo soc_type = {
    .name = TYPE_SG200X_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SG200XSoCState),
    .class_init = soc_class_init,
};

static void register_soc(void)
{
    type_register_static(&soc_type);
}
type_init(register_soc)
