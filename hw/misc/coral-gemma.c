/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local experiment: Coral instruction simulation with model-sized RAM. */
#include "qemu/osdep.h"
#include <dlfcn.h>
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/coral-gemma-abi.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_CORAL_GEMMA "coral-npu-gemma"
OBJECT_DECLARE_SIMPLE_TYPE(CoralGemmaState, CORAL_GEMMA)
struct CoralGemmaState {
    PCIDevice parent_obj;
    MemoryRegion regs, tcm, ddr;
    char *backend;
    void *library;
    CoralGemmaRunFn run;
    CoralGemmaRequest request;
    QemuThread thread;
    QemuMutex lock;
    QemuCond cond;
    QEMUBH *done_bh;
    bool stopping, has_job;
    int work_result;
    uint32_t status, result, irq_enable, irq_pending;
    uint64_t jobs;
};

static void gemma_irq(CoralGemmaState *s)
{
    pci_set_irq(&s->parent_obj, s->irq_enable && s->irq_pending);
}

static void gemma_done(void *opaque)
{
    CoralGemmaState *s = opaque;
    qemu_mutex_lock(&s->lock);
    s->result = s->work_result;
    s->status = 2 | (s->result == 1 ? 0 : 4);
    s->irq_pending = 1;
    s->jobs++;
    qemu_mutex_unlock(&s->lock);
    gemma_irq(s);
}

static void *gemma_worker(void *opaque)
{
    CoralGemmaState *s = opaque;
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

static void gemma_reset(DeviceState *dev)
{
    CoralGemmaState *s = CORAL_GEMMA(dev);
    qemu_mutex_lock(&s->lock);
    while (s->has_job) {
        qemu_cond_wait(&s->cond, &s->lock);
    }
    qemu_bh_cancel(s->done_bh);
    s->status = s->result = s->irq_enable = s->irq_pending = 0;
    s->jobs = 0;
    s->request.entry = 0;
    s->request.limit = 100000000;
    s->request.steps = s->request.ddr_reads = s->request.ddr_writes = 0;
    memset(s->request.error, 0, sizeof(s->request.error));
    qemu_mutex_unlock(&s->lock);
    gemma_irq(s);
}

static uint64_t gemma_read(void *opaque, hwaddr addr, unsigned size)
{
    CoralGemmaState *s = opaque;
    uint64_t value;
    qemu_mutex_lock(&s->lock);
    /* The worker owns request fields while BUSY. */
    if (addr >= 0x100 && addr < 0x200) {
        value = s->status == 1 ? 0 : ldl_le_p(s->request.error + addr - 0x100);
    } else {
        switch (addr) {
        case 0: value = 0x434f524c; break;
        case 4: value = CORAL_GEMMA_ABI; break;
        case 0xc: value = s->status; break;
        case 0x10: value = s->request.entry; break;
        case 0x14: value = s->request.limit; break;
        case 0x18: value = s->result; break;
        case 0x1c: value = s->status == 1 ? 0 : (uint32_t)s->request.steps; break;
        case 0x20: value = s->status == 1 ? 0 : s->request.steps >> 32; break;
        case 0x24: value = s->irq_enable; break;
        case 0x28: value = s->irq_pending; break;
        case 0x2c: value = s->jobs; break;
        case 0x30: value = s->status == 1 ? 0 : (uint32_t)s->request.ddr_reads; break;
        case 0x34: value = s->status == 1 ? 0 : s->request.ddr_reads >> 32; break;
        case 0x38: value = s->status == 1 ? 0 : (uint32_t)s->request.ddr_writes; break;
        case 0x3c: value = s->status == 1 ? 0 : s->request.ddr_writes >> 32; break;
        default: value = 0;
        }
    }
    qemu_mutex_unlock(&s->lock);
    return value;
}

static void gemma_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CoralGemmaState *s = opaque;
    if (addr == 8 && val == 2) {
        gemma_reset(DEVICE(s));
        return;
    }
    qemu_mutex_lock(&s->lock);
    if (addr == 0x24) {
        s->irq_enable = val & 1;
    } else if (addr == 0x28) {
        s->irq_pending &= ~val;
    } else if (!(s->status & 1)) {
        switch (addr) {
        case 0x10: s->request.entry = val; break;
        case 0x14: s->request.limit = val; break;
        case 8:
            if (val != 1) {
                break;
            }
            s->status = 1;
            s->result = s->irq_pending = 0;
            s->has_job = true;
            qemu_cond_broadcast(&s->cond);
            break;
        }
    }
    qemu_mutex_unlock(&s->lock);
    gemma_irq(s);
}

static const MemoryRegionOps gemma_ops = {
    .read = gemma_read, .write = gemma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void gemma_realize(PCIDevice *pdev, Error **errp)
{
    CoralGemmaState *s = CORAL_GEMMA(pdev);
    CoralGemmaVersionFn version;
    if (!s->backend || !*s->backend) {
        error_setg(errp, "coral-npu-gemma requires backend=/path/to/libcoral_gemma.so");
        return;
    }
    s->library = dlopen(s->backend, RTLD_NOW | RTLD_LOCAL);
    if (!s->library) {
        error_setg(errp, "Coral Gemma backend: %s", dlerror());
        return;
    }
    version = (CoralGemmaVersionFn)dlsym(s->library, "coral_gemma_abi");
    s->run = (CoralGemmaRunFn)dlsym(s->library, "coral_gemma_run");
    if (!version || !s->run || version() != CORAL_GEMMA_ABI) {
        error_setg(errp, "Coral Gemma backend ABI mismatch");
        dlclose(s->library);
        s->library = NULL;
        return;
    }
    memory_region_init_ram(&s->tcm, OBJECT(s), "coral-gemma-tcm",
                           CORAL_GEMMA_TCM_SIZE, &error_abort);
    memory_region_init_ram(&s->ddr, OBJECT(s), "coral-gemma-ddr",
                           CORAL_GEMMA_DDR_SIZE, &error_abort);
    s->request.tcm = memory_region_get_ram_ptr(&s->tcm);
    s->request.ddr = memory_region_get_ram_ptr(&s->ddr);
    qemu_mutex_init(&s->lock);
    qemu_cond_init(&s->cond);
    s->done_bh = qemu_bh_new(gemma_done, s);
    qemu_thread_create(&s->thread, "coral-gemma", gemma_worker, s,
                       QEMU_THREAD_JOINABLE);
    memory_region_init_io(&s->regs, OBJECT(s), &gemma_ops, s,
                          "coral-gemma-regs", 0x1000);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->tcm);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->ddr);
    pci_config_set_interrupt_pin(pdev->config, 1);
    gemma_reset(DEVICE(s));
}

static void gemma_exit(PCIDevice *pdev)
{
    CoralGemmaState *s = CORAL_GEMMA(pdev);
    qemu_mutex_lock(&s->lock);
    s->stopping = true;
    qemu_cond_broadcast(&s->cond);
    qemu_mutex_unlock(&s->lock);
    qemu_thread_join(&s->thread);
    qemu_bh_delete(s->done_bh);
    qemu_cond_destroy(&s->cond);
    qemu_mutex_destroy(&s->lock);
    dlclose(s->library);
}

static const VMStateDescription gemma_vmstate = {
    .name = "coral-npu-gemma", .unmigratable = true,
};
static const Property gemma_properties[] = {
    DEFINE_PROP_STRING("backend", CoralGemmaState, backend),
};
static void gemma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    pc->realize = gemma_realize;
    pc->exit = gemma_exit;
    pc->vendor_id = 0x1234;
    pc->device_id = 0xc0a2;
    pc->revision = 2;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->desc = "Coral MPACT functional simulator with 512 MiB external RAM";
    dc->vmsd = &gemma_vmstate;
    device_class_set_legacy_reset(dc, gemma_reset);
    device_class_set_props(dc, gemma_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}
static const TypeInfo gemma_info = {
    .name = TYPE_CORAL_GEMMA, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CoralGemmaState), .class_init = gemma_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE }, { },
    },
};
static void gemma_register_types(void) { type_register_static(&gemma_info); }
type_init(gemma_register_types)
