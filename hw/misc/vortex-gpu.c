/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local experimental PCI transport for the official Vortex SimX CP model. */
#include "qemu/osdep.h"
#include <dlfcn.h>
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_VORTEX_GPU "vortex-gpu"
OBJECT_DECLARE_SIMPLE_TYPE(VortexGPUState, VORTEX_GPU)
#define VRAM_WINDOW_SIZE (64 * MiB)
#define VRAM_WINDOW_BASE 0xf8000000ULL
#define TICK_BATCH 1024

/* The pinned SimX backend owns a process-wide SimPlatform singleton. */
static bool vortex_claimed;

struct VortexGPUState {
    PCIDevice parent_obj;
    MemoryRegion regs, window;
    char *backend;
    void *library, *handle;
    void *(*create)(void);
    void (*destroy)(void *);
    const char *(*build_info)(void);
    void (*reg_write)(void *, uint32_t, uint32_t);
    uint32_t (*reg_read)(void *, uint32_t);
    bool (*cp_tick)(void *);
    bool (*cp_has_work)(void *);
    bool (*gpu_tick)(void *);
    bool (*gpu_busy)(void *);
    void (*vram_write)(void *, uint64_t, const uint8_t *, uint32_t);
    void (*vram_read)(void *, uint64_t, uint8_t *, uint32_t);
    QEMUTimer *timer;
    uint64_t ticks, run_ticks;
    uint32_t limit, fault, irq_enable, irq_pending;
    bool active;
    char info[256];
};

static void vortex_irq(VortexGPUState *s)
{
    pci_set_irq(&s->parent_obj, s->irq_enable && s->irq_pending);
}

static bool vortex_busy(VortexGPUState *s)
{
    return s->handle && (s->cp_has_work(s->handle) || s->gpu_busy(s->handle));
}

static void vortex_tick(void *opaque)
{
    VortexGPUState *s = opaque;
    if (!s->handle || s->fault) {
        return;
    }
    for (unsigned i = 0; i < TICK_BATCH && vortex_busy(s); ++i) {
        s->cp_tick(s->handle);
        if (s->gpu_busy(s->handle)) {
            s->gpu_tick(s->handle);
        }
        ++s->ticks;
        if (++s->run_ticks >= s->limit && vortex_busy(s)) {
            s->fault = 1;
            break;
        }
    }
    if (vortex_busy(s) && !s->fault) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000);
    } else {
        s->active = false;
        s->irq_pending = 1;
        vortex_irq(s);
    }
}

static void vortex_arm(VortexGPUState *s)
{
    if (!s->fault && vortex_busy(s) && !s->active) {
        s->active = true;
        s->run_ticks = 0;
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

static void vortex_reset(DeviceState *dev)
{
    VortexGPUState *s = VORTEX_GPU(dev);
    if (!s->library) {
        return;
    }
    timer_del(s->timer);
    if (s->handle) {
        s->destroy(s->handle);
    }
    s->handle = s->create();
    s->ticks = s->run_ticks = 0;
    s->limit = 50000000;
    s->active = false;
    s->fault = s->handle ? 0 : 2;
    s->irq_enable = s->irq_pending = 0;
    vortex_irq(s);
}

static uint64_t vortex_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    VortexGPUState *s = opaque;
    if (addr < 0x200) {
        return s->handle ? s->reg_read(s->handle, addr) : UINT32_MAX;
    }
    if (addr >= 0x1100 && addr <= 0x11fc) {
        return ldl_le_p(s->info + addr - 0x1100);
    }
    switch (addr) {
    case 0x1000: return 0x56525458; /* VRTX */
    case 0x1004: return s->fault;
    case 0x1008: return s->ticks;
    case 0x100c: return s->ticks >> 32;
    case 0x1010: return s->limit;
    case 0x1018: return VRAM_WINDOW_BASE;
    case 0x101c: return VRAM_WINDOW_SIZE;
    case 0x1020: return s->irq_enable;
    case 0x1024: return s->irq_pending;
    default: return 0;
    }
}

static void vortex_reg_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    VortexGPUState *s = opaque;
    if (addr == 0x1014 && value == 1) {
        vortex_reset(DEVICE(s));
    } else if (addr == 0x1020) {
        s->irq_enable = value & 1;
        vortex_irq(s);
    } else if (addr == 0x1024) {
        s->irq_pending &= ~value;
        vortex_irq(s);
    } else if (addr == 0x1010 && !s->active) {
        s->limit = MAX(1, MIN(value, UINT32_MAX));
    } else if (addr < 0x200 && s->handle && !s->fault) {
        s->reg_write(s->handle, addr, value);
        vortex_arm(s);
    }
}

static uint64_t vortex_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    VortexGPUState *s = opaque;
    uint8_t bytes[8] = { 0 };
    uint64_t value = 0;
    if (!s->handle || addr > VRAM_WINDOW_SIZE - size) {
        return UINT64_MAX;
    }
    s->vram_read(s->handle, VRAM_WINDOW_BASE + addr, bytes, size);
    for (unsigned i = 0; i < size; ++i) {
        value |= (uint64_t)bytes[i] << (i * 8);
    }
    return value;
}

static void vortex_mem_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    VortexGPUState *s = opaque;
    uint8_t bytes[8];
    if (!s->handle || addr > VRAM_WINDOW_SIZE - size) {
        return;
    }
    for (unsigned i = 0; i < size; ++i) {
        bytes[i] = value >> (i * 8);
    }
    s->vram_write(s->handle, VRAM_WINDOW_BASE + addr, bytes, size);
}

static const MemoryRegionOps vortex_reg_ops = {
    .read = vortex_reg_read, .write = vortex_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps vortex_mem_ops = {
    .read = vortex_mem_read, .write = vortex_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void vortex_realize(PCIDevice *pdev, Error **errp)
{
    VortexGPUState *s = VORTEX_GPU(pdev);
    if (vortex_claimed) {
        error_setg(errp,
                   "Only one vortex-gpu is supported by this SimX backend");
        return;
    }
    if (!s->backend || !*s->backend) {
        error_setg(errp,
                   "vortex-gpu requires backend=/path/to/libvortex-gem5.so");
        return;
    }
    s->library = dlopen(s->backend, RTLD_NOW | RTLD_LOCAL);
    if (!s->library) {
        error_setg(errp, "Vortex SimX backend: %s", dlerror());
        return;
    }
#define LOAD(field, symbol) do { \
    *(void **)(&s->field) = dlsym(s->library, "vortex_gem5_" symbol); \
    if (!s->field) { \
        error_setg(errp, "Vortex backend missing symbol: " symbol); \
        goto fail; \
    } \
} while (0)
    LOAD(create, "create");
    LOAD(destroy, "destroy");
    LOAD(build_info, "build_info");
    LOAD(reg_write, "cp_mmio_write");
    LOAD(reg_read, "cp_mmio_read");
    LOAD(cp_tick, "cp_tick");
    LOAD(cp_has_work, "cp_has_work");
    LOAD(gpu_tick, "vortex_tick");
    LOAD(gpu_busy, "vortex_busy");
    LOAD(vram_write, "vram_write");
    LOAD(vram_read, "vram_read");
#undef LOAD
    g_strlcpy(s->info, s->build_info(), sizeof(s->info));
    s->handle = s->create();
    if (!s->handle) {
        error_setg(errp, "Vortex backend create failed");
        goto fail;
    }
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vortex_tick, s);
    s->limit = 50000000;
    memory_region_init_io(&s->regs, OBJECT(s), &vortex_reg_ops, s,
                          "vortex-cp", 0x2000);
    memory_region_init_io(&s->window, OBJECT(s), &vortex_mem_ops, s,
                          "vortex-vram-window", VRAM_WINDOW_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->window);
    pci_config_set_interrupt_pin(pdev->config, 1);
    vortex_claimed = true;
    return;
fail:
    dlclose(s->library);
    s->library = NULL;
}

static void vortex_exit(PCIDevice *pdev)
{
    VortexGPUState *s = VORTEX_GPU(pdev);
    timer_free(s->timer);
    s->destroy(s->handle);
    dlclose(s->library);
    vortex_claimed = false;
}

static const VMStateDescription vortex_vmstate = {
    .name = "vortex-gpu", .unmigratable = true,
};
static const Property vortex_properties[] = {
    DEFINE_PROP_STRING("backend", VortexGPUState, backend),
};
static void vortex_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = vortex_realize;
    pc->exit = vortex_exit;
    pc->vendor_id = 0x1234;
    pc->device_id = 0x5658;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->desc = "Experimental Vortex SimX compute GPU";
    dc->vmsd = &vortex_vmstate;
    device_class_set_legacy_reset(dc, vortex_reset);
    device_class_set_props(dc, vortex_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}
static const TypeInfo vortex_info = {
    .name = TYPE_VORTEX_GPU, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(VortexGPUState), .class_init = vortex_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { },
    },
};
static void vortex_register_types(void) { type_register_static(&vortex_info); }
type_init(vortex_register_types)
