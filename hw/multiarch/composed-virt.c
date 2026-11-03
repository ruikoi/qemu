/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local experiment: configurable multi-domain virt platform. */
#include "qemu/osdep.h"
#include "elf.h"
#include <libfdt.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/option_int.h"
#include "qemu/config-file.h"
#include "qemu/units.h"
#include "qemu/multiarch-runtime.h"
#include "hw/cpu/cluster.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/multiarch/composed-virt.h"
#include "system/device_tree.h"
#include "system/address-spaces.h"
#include "system/tcg.h"
#include "system/reset.h"
#include "system/system.h"
#include "migration/blocker.h"
#include "system/hostmem.h"

#define TYPE_COMPOSED_VIRT MACHINE_TYPE_NAME("multiarch-virt")
OBJECT_DECLARE_SIMPLE_TYPE(ComposedVirtState, COMPOSED_VIRT)
struct ComposedVirtState {
    MachineState parent_obj;
    GPtrArray *domains;
    Error *migration_blocker;
};

static QemuOptsList domain_opts = {
    .name = "domain",
    .head = QTAILQ_HEAD_INITIALIZER(domain_opts.head),
    .desc = {
        { .name = "platform", .type = QEMU_OPT_STRING },
        { .name = "cpu", .type = QEMU_OPT_STRING },
        { .name = "cpus", .type = QEMU_OPT_NUMBER },
        { .name = "ram", .type = QEMU_OPT_SIZE },
        { .name = "chardev", .type = QEMU_OPT_STRING },
        { }
    }
};

static QemuOptsList loader_opts = {
    .name = "domain-loader",
    .head = QTAILQ_HEAD_INITIALIZER(loader_opts.head),
    .desc = {
        { .name = "domain", .type = QEMU_OPT_STRING },
        { .name = "kernel", .type = QEMU_OPT_STRING },
        { .name = "initrd", .type = QEMU_OPT_STRING },
        { .name = "firmware", .type = QEMU_OPT_STRING },
        { .name = "append", .type = QEMU_OPT_STRING },
        { }
    }
};

static QemuOptsList map_opts = {
    .name = "domain-map",
    .head = QTAILQ_HEAD_INITIALIZER(map_opts.head),
    .desc = {
        { .name = "domain", .type = QEMU_OPT_STRING },
        { .name = "memory", .type = QEMU_OPT_STRING },
        { .name = "addr", .type = QEMU_OPT_NUMBER },
        { }
    }
};

static void composed_options(void)
{
    qemu_add_opts(&domain_opts);
    qemu_add_opts(&loader_opts);
    qemu_add_opts(&map_opts);
}
opts_init(composed_options)

void multiarch_virt_validate_machine(MachineState *machine)
{
    if ((!QTAILQ_EMPTY(&domain_opts.head) || !QTAILQ_EMPTY(&loader_opts.head) ||
         !QTAILQ_EMPTY(&map_opts.head)) &&
        !object_dynamic_cast(OBJECT(machine), TYPE_COMPOSED_VIRT)) {
        error_report(
            "-domain, -domain-loader and -domain-map require multiarch-virt");
        exit(EXIT_FAILURE);
    }
}

unsigned multiarch_virt_cpu_count(void)
{
    QemuOpts *opts;
    unsigned total = 0;
    QTAILQ_FOREACH(opts, &domain_opts.head, next)
    {
        uint64_t count = qemu_opt_get_number(opts, "cpus", 1);
        if (!count || count > 64 || total + count > 256) {
            error_report(
                "-domain requires 1..64 CPUs per domain, at most 256 total");
            exit(EXIT_FAILURE);
        }
        total += count;
    }
    return total;
}

void multiarch_virt_map_device(MultiarchVirtDomain *d, DeviceState *dev,
                               unsigned region, hwaddr addr)
{
    memory_region_add_subregion(
        &d->root, addr, sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), region));
}
void multiarch_virt_fdt_base(MultiarchVirtDomain *d)
{
    int size;
    d->fdt = create_device_tree(&size);
    if (!d->fdt) {
        error_report("cannot allocate domain device tree");
        exit(EXIT_FAILURE);
    }
    qemu_fdt_setprop_cell(d->fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(d->fdt, "/", "#size-cells", 2);
    qemu_fdt_setprop_string(d->fdt, "/", "compatible", "qemu,multiarch-virt");
    qemu_fdt_setprop_string(d->fdt, "/", "model", d->id);
    g_autofree char *mem = g_strdup_printf("/memory@%" PRIx64, d->ram_base);
    qemu_fdt_add_subnode(d->fdt, mem);
    qemu_fdt_setprop_string(d->fdt, mem, "device_type", "memory");
    qemu_fdt_setprop_sized_cells(d->fdt, mem, "reg", 2, d->ram_base, 2,
                                 d->ram_size);
    qemu_fdt_add_subnode(d->fdt, "/chosen");
    qemu_fdt_setprop_string(d->fdt, "/chosen", "bootargs",
                            d->append ? d->append
                                      : "console=ttyAMA0 rdinit=/init");
    qemu_fdt_add_subnode(d->fdt, "/cpus");
    qemu_fdt_setprop_cell(d->fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(d->fdt, "/cpus", "#size-cells", 0);
}

void multiarch_virt_load_firmware(MultiarchVirtDomain *d, unsigned elf_machine)
{
    uint64_t low = 0, high = 0;
    char magic[SELFMAG];
    FILE *f = fopen(d->firmware, "rb");
    if (!f) {
        error_report("domain %s: cannot open firmware %s", d->id, d->firmware);
        exit(EXIT_FAILURE);
    }
    bool elf = fread(magic, 1, sizeof(magic), f) == sizeof(magic) &&
               !memcmp(magic, ELFMAG, SELFMAG);
    fclose(f);
    d->entry = d->ram_base;
    if (elf) {
        bool vectors = !strcmp(d->platform, "arm-mcu");
        if (load_elf_as(d->firmware, NULL, NULL, NULL, &d->entry, &low, &high,
                        NULL, ELFDATA2LSB, elf_machine, 0, 0,
                        d->cpus[0]->as) < 0 ||
            !((low >= d->ram_base && high <= d->ram_base + d->ram_size) ||
              (vectors && high <= d->ram_size)) ||
            !(((d->entry & ~1ULL) >= d->ram_base &&
               (d->entry & ~1ULL) < d->ram_base + d->ram_size) ||
              (vectors && (d->entry & ~1ULL) < d->ram_size))) {
            error_report(
                "domain %s: ELF entry and segments must fit private RAM",
                d->id);
            exit(EXIT_FAILURE);
        }
    } else if (load_image_targphys_as(d->firmware, d->ram_base, d->ram_size,
                                      d->cpus[0]->as, &error_fatal) <= 0) {
        error_report("domain %s: invalid raw firmware", d->id);
        exit(EXIT_FAILURE);
    }
}

static MultiarchVirtDomain *find_domain(ComposedVirtState *s, const char *id)
{
    for (unsigned i = 0; i < s->domains->len; i++) {
        MultiarchVirtDomain *d = g_ptr_array_index(s->domains, i);
        if (id && !strcmp(id, d->id)) {
            return d;
        }
    }
    error_report("unknown domain '%s'", id ? id : "(missing)");
    exit(EXIT_FAILURE);
}
static void composed_init(MachineState *machine)
{
    ComposedVirtState *s = COMPOSED_VIRT(machine);
    QemuOpts *opts;
    unsigned first = 0, index = 0;
    if (!tcg_enabled() || !multiarch_virt_cpu_count()) {
        error_report("multiarch-virt requires TCG and at least one -domain");
        exit(EXIT_FAILURE);
    }
    if (machine->smp.cpus != 1 || machine->ram_size ||
        machine->kernel_filename || machine->initrd_filename ||
        machine->firmware || machine->dtb ||
        (machine->kernel_cmdline && *machine->kernel_cmdline) ||
        machine->cpu_type) {
        error_report(
            "multiarch-virt uses -domain/-domain-loader instead of "
            "-smp, -m, -cpu, -kernel, -initrd, -bios, -dtb or -append");
        exit(EXIT_FAILURE);
    }
    s->domains = g_ptr_array_new();
    QTAILQ_FOREACH(opts, &domain_opts.head, next)
    {
        MultiarchVirtDomain *d = g_new0(MultiarchVirtDomain, 1);
        const char *id = qemu_opts_id(opts), *cpu = qemu_opt_get(opts, "cpu");
        const char *platform = qemu_opt_get(opts, "platform");
        const char *chardev = qemu_opt_get(opts, "chardev");
        if (!id || !platform || !cpu || !qemu_opt_get(opts, "ram")) {
            error_report("-domain requires id, platform, cpu and ram");
            exit(EXIT_FAILURE);
        }
        bool arm =
            !strcmp(platform, "arm-virt") || !strcmp(platform, "arm-mcu");
        bool rv =
            !strcmp(platform, "riscv-virt") || !strcmp(platform, "riscv-mcu");
        if (!arm && !rv) {
            error_report("unknown domain platform %s", platform);
            exit(EXIT_FAILURE);
        }
        d->id = g_strdup(id);
        d->platform = g_strdup(platform);
        d->cpu_type = g_strdup_printf("%s-%s-cpu", cpu, arm ? "arm" : "riscv");
        d->num_cpus = qemu_opt_get_number(opts, "cpus", 1);
        d->ram_size = qemu_opt_get_size(opts, "ram", 0);
        d->ram_base = strstr(platform, "mcu") ? 0x10000000
                      : arm                   ? 0x40000000
                                              : 0x80000000;
        if (d->ram_size < MiB || d->ram_size > 16 * GiB || d->ram_size % MiB ||
            (strstr(platform, "mcu") &&
             (d->ram_size > 128 * MiB || d->num_cpus != 1))) {
            error_report("domain %s: RAM must be 1 MiB..16 GiB; MCU domains "
                         "require one CPU and at most 128 MiB",
                         id);
            exit(EXIT_FAILURE);
        }
        if (chardev) {
            d->serial = qemu_chr_find(chardev);
            if (!d->serial) {
                error_report("domain %s: unknown chardev %s", id, chardev);
                exit(EXIT_FAILURE);
            }
        }
        if (object_property_find(OBJECT(machine), id)) {
            error_report("domain id %s conflicts with a machine property", id);
            exit(EXIT_FAILURE);
        }
        d->index = index++;
        d->cpu_base = first;
        first += d->num_cpus;
        d->cpus = g_new0(CPUState *, d->num_cpus);
        g_ptr_array_add(s->domains, d);
    }
    QTAILQ_FOREACH(opts, &loader_opts.head, next)
    {
        MultiarchVirtDomain *d = find_domain(s, qemu_opt_get(opts, "domain"));
        if (d->kernel || d->firmware) {
            error_report("domain %s has more than one loader", d->id);
            exit(EXIT_FAILURE);
        }
        d->kernel = g_strdup(qemu_opt_get(opts, "kernel"));
        d->initrd = g_strdup(qemu_opt_get(opts, "initrd"));
        d->firmware = g_strdup(qemu_opt_get(opts, "firmware"));
        d->append = g_strdup(qemu_opt_get(opts, "append"));
        if ((!d->kernel && !d->firmware) ||
            (strstr(d->platform, "mcu") && (d->kernel || d->initrd)) ||
            (d->initrd && !d->kernel) ||
            (!strcmp(d->platform, "arm-virt") && d->kernel && d->firmware)) {
            error_report(
                "domain %s: invalid kernel/initrd/firmware combination", d->id);
            exit(EXIT_FAILURE);
        }
    }
    g_autofree const char **types = g_new0(const char *, s->domains->len);
    for (unsigned i = 0; i < s->domains->len; i++) {
        types[i] =
            ((MultiarchVirtDomain *)g_ptr_array_index(s->domains, i))->cpu_type;
    }
    multiarch_prepare_cpu_types(types, s->domains->len, &error_fatal);
    for (unsigned i = 0; i < s->domains->len; i++) {
        MultiarchVirtDomain *d = g_ptr_array_index(s->domains, i);
        d->cluster = object_new(TYPE_CPU_CLUSTER);
        object_property_add_child(OBJECT(machine), d->id, d->cluster);
        object_property_set_int(d->cluster, "cluster-id", d->index,
                                &error_fatal);
        memory_region_init(&d->root, d->cluster, "domain.memory", UINT64_MAX);
        g_autofree char *name = g_strdup_printf("%s.ram", d->id);
        memory_region_init_ram_flags_nomigrate(&d->ram, d->cluster, name,
                                               d->ram_size, 0, &error_fatal);
        memory_region_add_subregion(&d->root, d->ram_base, &d->ram);
        if (!strncmp(d->platform, "arm-", 4)) {
            multiarch_virt_arm_create(d);
        } else {
            multiarch_virt_riscv_create(d);
        }
        qdev_realize(DEVICE(d->cluster), NULL, &error_fatal);
        for (unsigned j = 0; j < d->num_cpus; j++) {
            tcg_update_cflags(d->cpus[j]);
        }
    }
    QTAILQ_FOREACH(opts, &map_opts.head, next)
    {
        MultiarchVirtDomain *d = find_domain(s, qemu_opt_get(opts, "domain"));
        const char *name = qemu_opt_get(opts, "memory");
        Object *object =
            name
                ? object_resolve_path_component(object_get_objects_root(), name)
                : NULL;
        if (!object || !object_dynamic_cast(object, TYPE_MEMORY_BACKEND) ||
            !qemu_opt_get(opts, "addr")) {
            error_report(
                "-domain-map requires domain, memory backend id and addr");
            exit(EXIT_FAILURE);
        }
        MemoryRegion *mem =
            host_memory_backend_get_memory(MEMORY_BACKEND(object));
        uint64_t addr = qemu_opt_get_number(opts, "addr", 0),
                 size = memory_region_size(mem);
        if (addr > UINT64_MAX - size) {
            error_report("domain mapping address overflow");
            exit(EXIT_FAILURE);
        }
        MemoryRegionSection section = memory_region_find(&d->root, addr, size);
        if (section.mr) {
            memory_region_unref(section.mr);
            error_report("domain %s: shared mapping overlaps RAM or MMIO",
                         d->id);
            exit(EXIT_FAILURE);
        }
        MemoryRegion *alias = g_new0(MemoryRegion, 1);
        g_autofree char *alias_name =
            g_strdup_printf("%s@%" PRIx64, name, addr);
        memory_region_init_alias(alias, d->cluster, alias_name, mem, 0, size);
        memory_region_add_subregion(&d->root, addr, alias);
        host_memory_backend_set_mapped(MEMORY_BACKEND(object), true);
    }
    for (unsigned i = 0; i < s->domains->len; i++) {
        MultiarchVirtDomain *d = g_ptr_array_index(s->domains, i);
        if (!strncmp(d->platform, "arm-", 4)) {
            multiarch_virt_arm_boot(d, machine);
        } else {
            multiarch_virt_riscv_boot(d, machine);
        }
    }
    error_setg(&s->migration_blocker,
               "multiarch-virt domains do not support migration");
    migrate_add_blocker(&s->migration_blocker, &error_fatal);
}
static void composed_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Configurable isolated ARM/RISC-V virt execution domains";
    mc->init = composed_init;
    mc->min_cpus = mc->max_cpus = mc->default_cpus = 1;
    mc->default_ram_size = 0;
    mc->minimum_page_bits = 10;
    mc->default_cpu_type = NULL;
    mc->get_default_cpu_type = NULL;
    mc->no_serial = true;
    mc->no_parallel = true;
}
static const TypeInfo composed_info = {
    .name = TYPE_COMPOSED_VIRT,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(ComposedVirtState),
    .class_init = composed_class_init,
};
static void composed_register(void)
{
    type_register_static(&composed_info);
}
type_init(composed_register)
