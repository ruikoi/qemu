/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local single-process ARM/RISC-V experiment, not an upstream contribution. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/cpu/cluster.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "qemu/multiarch-runtime.h"
#include "system/address-spaces.h"
#include "system/tcg.h"
#include "migration/blocker.h"

#define TYPE_HETERO_MACHINE MACHINE_TYPE_NAME("multiarch")
OBJECT_DECLARE_SIMPLE_TYPE(HeteroMachineState, HETERO_MACHINE)

struct HeteroMachineState {
    MachineState parent_obj;
    char *cpu_type[2];
    MemoryRegion root[2];
    MemoryRegion private_ram[2];
    MemoryRegion boot_alias[2];
    Clock *cpu_clock[2];
    MemoryRegion shared;
    MemoryRegion shared_view[2];
    MemoryRegion monitor_private[2];
    MemoryRegion monitor_shared;
    Error *migration_blocker;
};

typedef struct HeteroProfile {
    const char *name;
    const char *qom_type;
    bool arm_mcu;
    bool riscv_mcu;
} HeteroProfile;

static const HeteroProfile profiles[] = {
    { "cortex-a53-arm-cpu", "cortex-a53-arm-cpu" },
    { "rv64-riscv-cpu", "rv64-riscv-cpu" },
    { "cortex-m3-arm-cpu", "cortex-m3-arm-cpu", true },
    { "rv32-mcu", "rv32-riscv-cpu", false, true },
};

static const HeteroProfile *find_profile(const char *name)
{
    for (unsigned i = 0; i < ARRAY_SIZE(profiles); i++) {
        if (!strcmp(name, profiles[i].name)) {
            return &profiles[i];
        }
    }
    error_report("unsupported hetero CPU profile: %s", name);
    exit(EXIT_FAILURE);
}

static void hetero_init(MachineState *machine)
{
    HeteroMachineState *s = HETERO_MACHINE(machine);
    CPUState *cpu[2];
    Object *cluster[2];
    Object *mcu[2] = { NULL, NULL };
    const HeteroProfile *profile[2];
    const char *cpu_types[2];

    if (!tcg_enabled()) {
        error_report("hetero requires the TCG accelerator");
        exit(EXIT_FAILURE);
    }
    if (machine->ram_size != 0x100000) {
        error_report("hetero currently has fixed 1 MiB private RAM per CPU");
        exit(EXIT_FAILURE);
    }
    if (machine->kernel_filename || machine->firmware || machine->cpu_type) {
        error_report("hetero uses cpu0/cpu1 properties and per-CPU loader devices");
        exit(EXIT_FAILURE);
    }
    error_setg(&s->migration_blocker, "hetero prototype does not support migration");
    migrate_add_blocker(&s->migration_blocker, &error_fatal);
    for (unsigned i = 0; i < 2; i++) {
        profile[i] = find_profile(s->cpu_type[i]);
        cpu_types[i] = profile[i]->qom_type;
    }
    multiarch_prepare_cpu_types(cpu_types, ARRAY_SIZE(cpu_types), &error_fatal);
    memory_region_init_ram_flags_nomigrate(&s->shared, OBJECT(s), "hetero.shared",
                                         0x10000, 0, &error_fatal);
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = NULL;
        name = g_strdup_printf("cluster%u", i);
        cluster[i] = object_new(TYPE_CPU_CLUSTER);
        object_property_add_child(OBJECT(s), name, cluster[i]);
        object_property_set_int(cluster[i], "cluster-id", i, &error_fatal);

        memory_region_init(&s->root[i], OBJECT(s), name, UINT64_MAX);
        g_free(name);
        name = g_strdup_printf("hetero.private%u", i);
        memory_region_init_ram_flags_nomigrate(&s->private_ram[i], OBJECT(s), name,
                                             0x100000, 0, &error_fatal);
        memory_region_add_subregion(&s->root[i], 0x10000000, &s->private_ram[i]);
        g_free(name);
        name = g_strdup_printf("hetero.shared-view%u", i);
        memory_region_init_alias(&s->shared_view[i], OBJECT(s), name,
                                 &s->shared, 0, 0x10000);
        memory_region_add_subregion(&s->root[i],
                                    i ? 0x30000000 : 0x20000000,
                                    &s->shared_view[i]);
        if (profile[i]->arm_mcu) {
            /* Independent vector table, NVIC and SysTick in this CPU view. */
            g_free(name);
            name = g_strdup_printf("boot-vectors%u", i);
            memory_region_init_alias(&s->boot_alias[i], OBJECT(s), name,
                                     &s->private_ram[i], 0, 0x100000);
            memory_region_add_subregion(&s->root[i], 0, &s->boot_alias[i]);
            mcu[i] = object_new("armv7m");
            object_property_add_child(cluster[i], "mcu", mcu[i]);
            object_property_set_str(mcu[i], "cpu-type", profile[i]->qom_type,
                                    &error_fatal);
            object_property_set_int(mcu[i], "num-irq", 32, &error_fatal);
            object_property_set_link(mcu[i], "memory", OBJECT(&s->root[i]),
                                     &error_fatal);
            g_free(name);
            name = g_strdup_printf("cpu-clock%u", i);
            s->cpu_clock[i] = clock_new(OBJECT(s), name);
            clock_set_hz(s->cpu_clock[i], 100000000);
            qdev_connect_clock_in(DEVICE(mcu[i]), "cpuclk", s->cpu_clock[i]);
        } else {
            cpu[i] = CPU(object_new(profile[i]->qom_type));
            object_property_add_child(cluster[i], "cpu", OBJECT(cpu[i]));
            object_property_set_link(OBJECT(cpu[i]), "memory", OBJECT(&s->root[i]),
                                     &error_fatal);
            if (profile[i]->riscv_mcu) {
                object_property_set_bool(OBJECT(cpu[i]), "mmu", false,
                                         &error_fatal);
                object_property_set_bool(OBJECT(cpu[i]), "s", false,
                                         &error_fatal);
                object_property_set_bool(OBJECT(cpu[i]), "h", false,
                                         &error_fatal);
            }
            hetero_cpu_set_arch_id(cpu[i], i);
        }
        /* Monitor-only aliases; neither CPU maps the system address space. */
        g_free(name);
        name = g_strdup_printf("hetero.monitor-private%u", i);
        memory_region_init_alias(&s->monitor_private[i], OBJECT(s), name,
                                 &s->private_ram[i], 0, 0x100000);
        memory_region_add_subregion(get_system_memory(),
                                    0x40000000 + i * 0x1000000,
                                    &s->monitor_private[i]);
    }
    memory_region_init_alias(&s->monitor_shared, OBJECT(s), "hetero.monitor-shared",
                             &s->shared, 0, 0x10000);
    memory_region_add_subregion(get_system_memory(), 0x50000000,
                                &s->monitor_shared);
    for (unsigned i = 0; i < 2; i++) {
        if (mcu[i]) {
            sysbus_realize(SYS_BUS_DEVICE(mcu[i]), &error_fatal);
            cpu[i] = CPU(object_resolve_path_component(mcu[i], "cpu"));
            object_unref(mcu[i]);
        } else {
            qdev_realize(DEVICE(cpu[i]), NULL, &error_fatal);
            object_unref(OBJECT(cpu[i]));
        }
        qdev_realize(DEVICE(cluster[i]), NULL, &error_fatal);
        tcg_update_cflags(cpu[i]);
        object_unref(cluster[i]);
    }
    for (unsigned i = 0; i < 2; i++) {
        if (object_dynamic_cast(OBJECT(cpu[i]), "riscv-cpu")) {
            DeviceState *swi = qdev_new("riscv.aclint.swi");
            g_autofree char *name = g_strdup_printf("mswi%u", i);
            object_property_add_child(OBJECT(s), name, OBJECT(swi));
            qdev_prop_set_uint32(swi, "hartid-base", i);
            qdev_prop_set_uint32(swi, "num-harts", 1);
            sysbus_realize(SYS_BUS_DEVICE(swi), &error_fatal);
            memory_region_add_subregion(&s->root[i], 0x02000000,
                      sysbus_mmio_get_region(SYS_BUS_DEVICE(swi), 0));
            qdev_connect_gpio_out(swi, 0, qdev_get_gpio_in(DEVICE(cpu[i]), 3));
            object_unref(OBJECT(swi));
        }
    }
}

static char *get_cpu0(Object *obj, Error **errp)
{
    return g_strdup(HETERO_MACHINE(obj)->cpu_type[0]);
}
static char *get_cpu1(Object *obj, Error **errp)
{
    return g_strdup(HETERO_MACHINE(obj)->cpu_type[1]);
}
static void set_cpu0(Object *obj, const char *value, Error **errp)
{
    HeteroMachineState *s = HETERO_MACHINE(obj);
    g_free(s->cpu_type[0]);
    s->cpu_type[0] = g_strdup(value);
}
static void set_cpu1(Object *obj, const char *value, Error **errp)
{
    HeteroMachineState *s = HETERO_MACHINE(obj);
    g_free(s->cpu_type[1]);
    s->cpu_type[1] = g_strdup(value);
}
static void hetero_instance_init(Object *obj)
{
    HeteroMachineState *s = HETERO_MACHINE(obj);
    s->cpu_type[0] = g_strdup("cortex-a53-arm-cpu");
    s->cpu_type[1] = g_strdup("rv64-riscv-cpu");
}
static void hetero_instance_finalize(Object *obj)
{
    HeteroMachineState *s = HETERO_MACHINE(obj);
    g_free(s->cpu_type[0]);
    g_free(s->cpu_type[1]);
}
static void hetero_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->alias = "hetero";
    mc->desc = "Local experimental ARM/RISC-V CPU/MCU composition machine";
    mc->init = hetero_init;
    mc->min_cpus = mc->max_cpus = mc->default_cpus = 2;
    /* Generic loader bounds raw images using this per-CPU private RAM size. */
    mc->default_ram_size = 0x100000;
    /* Cortex-M requires 1 KiB TCG granules; ISA page tables stay unchanged. */
    mc->minimum_page_bits = 10;
    mc->no_serial = mc->no_parallel = mc->no_floppy = mc->no_cdrom = true;
    object_class_property_add_str(oc, "cpu0", get_cpu0, set_cpu0);
    object_class_property_add_str(oc, "cpu1", get_cpu1, set_cpu1);
}
static const TypeInfo hetero_type = {
    .name = TYPE_HETERO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(HeteroMachineState),
    .instance_init = hetero_instance_init,
    .instance_finalize = hetero_instance_finalize,
    .class_init = hetero_class_init,
};
static void hetero_register_types(void)
{
    type_register_static(&hetero_type);
}
type_init(hetero_register_types)
