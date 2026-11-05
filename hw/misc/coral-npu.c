/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental PCI transport for the official Coral V2 functional simulator. */
#include "qemu/osdep.h"
#include <dlfcn.h>
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qom/object.h"
#include "hw/misc/coral-npu-abi.h"

#define TYPE_CORAL_NPU "coral-npu"
OBJECT_DECLARE_SIMPLE_TYPE(CoralNPUState, CORAL_NPU)
#define SRAM_SIZE 0x800000
#define EXT_OFFSET 0x400000
#define STATUS_BUSY 1
#define STATUS_DONE 2
#define STATUS_ERROR 4

struct CoralNPUState {
    PCIDevice parent_obj;
    MemoryRegion regs, sram;
    char *backend;
    void *library;
    CoralRunFn run;
    uint8_t *memory, *work;
    CoralRequest request;
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    QEMUBH *done_bh;
    bool stopping, has_job;
    int work_result;
    uint32_t entry, limit, status, result, irq_enable, irq_pending;
    uint64_t steps;
    char error[256];
};

static void coral_irq(CoralNPUState *s)
{
    pci_set_irq(&s->parent_obj, s->irq_enable && s->irq_pending);
}

static void coral_done(void *opaque)
{
    CoralNPUState *s = opaque;
    qemu_mutex_lock(&s->lock);
    memcpy(s->memory, s->work, SRAM_SIZE);
    s->result = s->work_result;
    s->steps = s->request.steps;
    memcpy(s->error, s->request.error, sizeof(s->error));
    s->status = STATUS_DONE | (s->result == CORAL_OK ? 0 : STATUS_ERROR);
    s->irq_pending = 1;
    qemu_mutex_unlock(&s->lock);
    coral_irq(s);
}

static void *coral_worker(void *opaque)
{
    CoralNPUState *s = opaque;
    qemu_mutex_lock(&s->lock);
    for (;;) {
        while (!s->has_job && !s->stopping) {
            qemu_cond_wait(&s->cond, &s->lock);
        }
        if (s->stopping) {
            break;
        }
        qemu_mutex_unlock(&s->lock);
        int result = s->run(&s->request);
        qemu_mutex_lock(&s->lock);
        s->work_result = result;
        s->has_job = false;
        qemu_bh_schedule(s->done_bh);
        qemu_cond_broadcast(&s->cond);
    }
    qemu_mutex_unlock(&s->lock);
    return NULL;
}

static void coral_reset(DeviceState *dev)
{
    CoralNPUState *s = CORAL_NPU(dev);
    qemu_mutex_lock(&s->lock);
    while (s->has_job) {
        qemu_cond_wait(&s->cond, &s->lock);
    }
    qemu_bh_cancel(s->done_bh);
    qemu_mutex_unlock(&s->lock);
    memset(s->memory, 0, SRAM_SIZE);
    memset(s->error, 0, sizeof(s->error));
    s->entry = s->status = s->result = s->irq_enable = s->irq_pending = 0;
    s->steps = 0;
    s->limit = 100000;
    coral_irq(s);
}

static uint64_t coral_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    CoralNPUState *s = opaque;
    if (addr >= 0x100 && addr < 0x200) {
        return ldl_le_p(s->error + addr - 0x100);
    }
    switch (addr) {
    case 0x00: return 0x434f524c; /* CORL */
    case 0x04: return CORAL_ABI_VERSION;
    case 0x0c: return s->status;
    case 0x10: return s->entry;
    case 0x14: return s->limit;
    case 0x18: return s->result;
    case 0x1c: return s->steps;
    case 0x20: return s->steps >> 32;
    case 0x24: return s->irq_enable;
    case 0x28: return s->irq_pending;
    default: return 0;
    }
}

static void coral_reg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    CoralNPUState *s = opaque;
    if (addr == 0x24) {
        s->irq_enable = val & 1;
        coral_irq(s);
        return;
    }
    if (addr == 0x28) {
        s->irq_pending &= ~val;
        coral_irq(s);
        return;
    }
    if (addr == 0x08 && val == 2) {
        coral_reset(DEVICE(s));
        return;
    }
    if (s->status & STATUS_BUSY) {
        return;
    }
    switch (addr) {
    case 0x10: s->entry = val; break;
    case 0x14: s->limit = val; break;
    case 0x08:
        if (val != 1) {
            break;
        }
        s->status = STATUS_BUSY;
        s->result = s->irq_pending = 0;
        s->steps = 0;
        memset(s->error, 0, sizeof(s->error));
        coral_irq(s);
        qemu_mutex_lock(&s->lock);
        memcpy(s->work, s->memory, SRAM_SIZE);
        s->request.entry = s->entry;
        s->request.limit = s->limit;
        s->has_job = true;
        qemu_cond_broadcast(&s->cond);
        qemu_mutex_unlock(&s->lock);
        break;
    }
}

static bool coral_valid(hwaddr addr, unsigned size)
{
    return (addr < CORAL_ITCM_SIZE && size <= CORAL_ITCM_SIZE - addr) ||
           (addr >= 0x10000 && addr < 0x18000 && size <= 0x18000 - addr) ||
           (addr >= EXT_OFFSET && addr < SRAM_SIZE && size <= SRAM_SIZE - addr);
}

static uint64_t coral_mem_read(void *opaque, hwaddr addr, unsigned size)
{
    CoralNPUState *s = opaque;
    uint64_t value = 0;
    if ((s->status & STATUS_BUSY) || !coral_valid(addr, size)) {
        return UINT64_MAX;
    }
    for (unsigned i = 0; i < size; i++) {
        value |= (uint64_t)s->memory[addr + i] << (8 * i);
    }
    return value;
}

static void coral_mem_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    CoralNPUState *s = opaque;
    if ((s->status & STATUS_BUSY) || !coral_valid(addr, size)) {
        return;
    }
    for (unsigned i = 0; i < size; i++) {
        s->memory[addr + i] = value >> (8 * i);
    }
}

static const MemoryRegionOps coral_reg_ops = {
    .read = coral_reg_read, .write = coral_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps coral_mem_ops = {
    .read = coral_mem_read, .write = coral_mem_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void coral_realize(PCIDevice *pdev, Error **errp)
{
    CoralNPUState *s = CORAL_NPU(pdev);
    CoralVersionFn version;
    if (!s->backend || !*s->backend) {
        error_setg(errp, "coral-npu requires backend=/path/to/libcoral_qemu.so");
        return;
    }
    s->library = dlopen(s->backend, RTLD_NOW | RTLD_LOCAL);
    if (!s->library) {
        error_setg(errp, "Coral backend: %s", dlerror());
        return;
    }
    version = (CoralVersionFn)dlsym(s->library, "coral_qemu_abi");
    s->run = (CoralRunFn)dlsym(s->library, "coral_qemu_run");
    if (!version || !s->run || version() != CORAL_ABI_VERSION) {
        error_setg(errp, "Coral backend ABI mismatch");
        dlclose(s->library);
        s->library = NULL;
        return;
    }
    s->memory = g_malloc0(SRAM_SIZE);
    s->work = g_malloc0(SRAM_SIZE);
    s->request.itcm = s->work;
    s->request.dtcm = s->work + 0x10000;
    s->request.ext = s->work + EXT_OFFSET;
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->cond);
    s->done_bh = qemu_bh_new(coral_done, s);
    qemu_thread_create(&s->thread, "coral-npu", coral_worker, s,
                       QEMU_THREAD_JOINABLE);
    memory_region_init_io(&s->regs, OBJECT(s), &coral_reg_ops, s,
                          "coral-regs", 0x1000);
    memory_region_init_io(&s->sram, OBJECT(s), &coral_mem_ops, s,
                          "coral-sram", SRAM_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->sram);
    pci_config_set_interrupt_pin(pdev->config, 1);
    coral_reset(DEVICE(s));
}

static void coral_exit(PCIDevice *pdev)
{
    CoralNPUState *s = CORAL_NPU(pdev);
    qemu_mutex_lock(&s->lock);
    s->stopping = true;
    qemu_cond_broadcast(&s->cond);
    qemu_mutex_unlock(&s->lock);
    qemu_thread_join(&s->thread);
    qemu_bh_delete(s->done_bh);
    qemu_cond_destroy(&s->cond);
    qemu_mutex_destroy(&s->lock);
    g_free(s->memory);
    g_free(s->work);
    dlclose(s->library);
}

static const VMStateDescription coral_vmstate = {
    .name = "coral-npu",
    .unmigratable = true,
};

static const Property coral_properties[] = {
    DEFINE_PROP_STRING("backend", CoralNPUState, backend),
};

static void coral_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = coral_realize;
    pc->exit = coral_exit;
    pc->vendor_id = 0x1234;
    pc->device_id = 0xc0a1;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->desc = "Experimental Coral V2 functional NPU";
    dc->vmsd = &coral_vmstate;
    device_class_set_legacy_reset(dc, coral_reset);
    device_class_set_props(dc, coral_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo coral_info = {
    .name = TYPE_CORAL_NPU,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CoralNPUState),
    .class_init = coral_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { },
    },
};
static void coral_register_types(void) { type_register_static(&coral_info); }
type_init(coral_register_types)
