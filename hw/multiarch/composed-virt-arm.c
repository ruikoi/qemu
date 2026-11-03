/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ISA-local implementation of configurable ARM domains. */
#include "qemu/osdep.h"
#include "elf.h"
#include <libfdt.h>
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qobject/qlist.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/multiarch/composed-virt.h"
#include "hw/multiarch/domain.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "target/arm/gtimer.h"
#include "target/arm/multiprocessing.h"
#include "qemu/multiarch-runtime.h"

typedef struct ArmDomainBoot {
    struct arm_boot_info info;
    MultiarchVirtDomain *domain;
} ArmDomainBoot;

static void *arm_domain_dtb(const struct arm_boot_info *info, int *size)
{
    ArmDomainBoot *boot = container_of(info, ArmDomainBoot, info);
    *size = fdt_totalsize(boot->domain->fdt);
    return g_memdup2(boot->domain->fdt, *size);
}
static void arm_domain_reset(void *opaque)
{
    MultiarchVirtDomain *d = opaque;
    for (unsigned i = 0; i < d->num_cpus; i++) {
        cpu_reset(d->cpus[i]);
        if (d->firmware && strcmp(d->cpu_type, "cortex-m3-arm-cpu")) {
            cpu_set_pc(d->cpus[i], d->entry);
        }
        if (!d->firmware && !d->kernel) {
            d->cpus[i]->halted = 1;
        }
    }
}

void multiarch_virt_arm_create(MultiarchVirtDomain *d)
{
    bool application = !strcmp(d->platform, "arm-virt");
    bool m3 = !strcmp(d->cpu_type, "cortex-m3-arm-cpu");
    if ((application && strcmp(d->cpu_type, "cortex-a53-arm-cpu") &&
         strcmp(d->cpu_type, "cortex-a55-arm-cpu")) ||
        (!application && !m3 && strcmp(d->cpu_type, "cortex-r5-arm-cpu") &&
         strcmp(d->cpu_type, "cortex-r5f-arm-cpu"))) {
        error_report("domain %s: arm-virt supports cortex-a53/a55; "
                     "arm-mcu supports cortex-m3/r5/r5f",
                     d->id);
        exit(EXIT_FAILURE);
    }
    if (!application) {
        memory_region_init_alias(&d->vectors, d->cluster, "vectors", &d->ram, 0,
                                 d->ram_size);
        memory_region_add_subregion(&d->root, 0, &d->vectors);
    }
    if (m3) {
        MultiarchDomainConfig config = {
            .id = d->cpu_base,
            .num_irqs = 32,
            .uart = 0x40000000,
            .clock_hz = 50000000,
            .serial = d->serial,
        };
        const MultiarchCoreProfile *profile =
            multiarch_core_profile_find(d->cpu_type);
        d->cpus[0] = profile->create_cpu(d->cluster, &d->root, &config);
        profile->create_devices(d->cpus[0], &d->root, &config);
        return;
    }
    for (unsigned i = 0; i < d->num_cpus; i++) {
        CPUState *cs = CPU(object_new(d->cpu_type));
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        object_property_add_child(d->cluster, name, OBJECT(cs));
        object_property_set_link(OBJECT(cs), "memory", OBJECT(&d->root),
                                 &error_fatal);
        object_property_set_int(OBJECT(cs), "mp-affinity", d->cpu_base + i,
                                &error_fatal);
        if (application) {
            object_property_set_bool(OBJECT(cs), "has_el3", false,
                                     &error_fatal);
            object_property_set_bool(OBJECT(cs), "has_el2", true, &error_fatal);
            object_property_set_int(OBJECT(cs), "psci-conduit",
                                    QEMU_PSCI_CONDUIT_SMC, &error_fatal);
            object_property_set_bool(OBJECT(cs), "start-powered-off", i != 0,
                                     &error_fatal);
        }
        qdev_realize(DEVICE(cs), NULL, &error_fatal);
        d->cpus[i] = cs;
        object_unref(OBJECT(cs));
    }
    DeviceState *gic = qdev_new(application ? "arm-gicv3" : "arm_gic");
    qdev_prop_set_uint32(gic, "num-cpu", d->num_cpus);
    qdev_prop_set_uint32(gic, "first-cpu-index", d->cpu_base);
    qdev_prop_set_uint32(gic, "num-irq", 288);
    qdev_prop_set_uint32(gic, "revision", application ? 3 : 2);
    if (application) {
        QList *counts = qlist_new();
        qlist_append_int(counts, d->num_cpus);
        qdev_prop_set_array(gic, "redist-region-count", counts);
        qdev_prop_set_bit(gic, "has-lpi", false);
    }
    sysbus_realize(SYS_BUS_DEVICE(gic), &error_fatal);
    multiarch_virt_map_device(d, gic, 0, 0x08000000);
    multiarch_virt_map_device(d, gic, 1, application ? 0x080a0000 : 0x08010000);
    for (unsigned i = 0; i < d->num_cpus; i++) {
        DeviceState *cpu = DEVICE(d->cpus[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i,
                           qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + d->num_cpus,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        if (application) {
            const unsigned timer_irqs[] = {30, 27, 26, 29, 28, 20, 19};
            for (unsigned t = 0; t < ARRAY_SIZE(timer_irqs); t++) {
                qdev_connect_gpio_out(
                    cpu, t,
                    qdev_get_gpio_in(gic, 256 + i * 32 + timer_irqs[t]));
            }
            qdev_connect_gpio_out_named(
                cpu, "gicv3-maintenance-interrupt", 0,
                qdev_get_gpio_in(gic, 256 + i * 32 + 25));
            qdev_connect_gpio_out_named(
                cpu, "pmu-interrupt", 0,
                qdev_get_gpio_in(gic, 256 + i * 32 + 23));
            sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 2 * d->num_cpus,
                               qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
            sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 3 * d->num_cpus,
                               qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
            sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 4 * d->num_cpus,
                               qdev_get_gpio_in(cpu, ARM_CPU_NMI));
            sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + 5 * d->num_cpus,
                               qdev_get_gpio_in(cpu, ARM_CPU_VINMI));
        }
    }
    DeviceState *uart = qdev_new("pl011");
    qdev_prop_set_chr(uart, "chardev", d->serial);
    sysbus_realize(SYS_BUS_DEVICE(uart), &error_fatal);
    multiarch_virt_map_device(d, uart, 0, 0x09000000);
    sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0, qdev_get_gpio_in(gic, 0));
    if (!application) {
        DeviceState *timer = qdev_new("sp804");
        sysbus_realize(SYS_BUS_DEVICE(timer), &error_fatal);
        multiarch_virt_map_device(d, timer, 0, 0x09010000);
        sysbus_connect_irq(SYS_BUS_DEVICE(timer), 0, qdev_get_gpio_in(gic, 1));
        return;
    }
    multiarch_virt_fdt_base(d);
    for (unsigned i = 0; i < d->num_cpus; i++) {
        g_autofree char *node =
            g_strdup_printf("/cpus/cpu@%x", d->cpu_base + i);
        qemu_fdt_add_subnode(d->fdt, node);
        qemu_fdt_setprop_string(d->fdt, node, "device_type", "cpu");
        qemu_fdt_setprop_string(d->fdt, node, "compatible",
                                ARM_CPU(d->cpus[i])->dtb_compatible);
        qemu_fdt_setprop_cell(d->fdt, node, "reg", d->cpu_base + i);
        qemu_fdt_setprop_string(d->fdt, node, "enable-method", "psci");
    }
    qemu_fdt_add_subnode(d->fdt, "/intc@8000000");
    qemu_fdt_setprop_string(d->fdt, "/intc@8000000", "compatible",
                            "arm,gic-v3");
    qemu_fdt_setprop_cell(d->fdt, "/intc@8000000", "#interrupt-cells", 3);
    qemu_fdt_setprop(d->fdt, "/intc@8000000", "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(d->fdt, "/intc@8000000", "phandle", 1);
    qemu_fdt_setprop_sized_cells(d->fdt, "/intc@8000000", "reg", 2, 0x08000000,
                                 2, 0x10000, 2, 0x080a0000, 2,
                                 d->num_cpus * 0x20000);
    qemu_fdt_setprop_cell(d->fdt, "/", "interrupt-parent", 1);
    qemu_fdt_add_subnode(d->fdt, "/timer");
    qemu_fdt_setprop_string(d->fdt, "/timer", "compatible", "arm,armv8-timer");
    qemu_fdt_setprop(d->fdt, "/timer", "always-on", NULL, 0);
    qemu_fdt_setprop_cells(d->fdt, "/timer", "interrupts", 1, 13, 4, 1, 14, 4,
                           1, 11, 4, 1, 10, 4);
    qemu_fdt_add_subnode(d->fdt, "/apb-clock");
    qemu_fdt_setprop_string(d->fdt, "/apb-clock", "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(d->fdt, "/apb-clock", "#clock-cells", 0);
    qemu_fdt_setprop_cell(d->fdt, "/apb-clock", "clock-frequency", 24000000);
    qemu_fdt_setprop_cell(d->fdt, "/apb-clock", "phandle", 2);
    qemu_fdt_add_subnode(d->fdt, "/pl011@9000000");
    const char compat[] = "arm,pl011\0arm,primecell";
    const char clocks[] = "uartclk\0apb_pclk";
    qemu_fdt_setprop(d->fdt, "/pl011@9000000", "compatible", compat,
                     sizeof(compat));
    qemu_fdt_setprop_sized_cells(d->fdt, "/pl011@9000000", "reg", 2, 0x09000000,
                                 2, 0x1000);
    qemu_fdt_setprop_cells(d->fdt, "/pl011@9000000", "interrupts", 0, 0, 4);
    qemu_fdt_setprop_cells(d->fdt, "/pl011@9000000", "clocks", 2, 2);
    qemu_fdt_setprop(d->fdt, "/pl011@9000000", "clock-names", clocks,
                     sizeof(clocks));
    qemu_fdt_setprop_string(d->fdt, "/chosen", "stdout-path", "/pl011@9000000");
}

void multiarch_virt_arm_boot(MultiarchVirtDomain *d, MachineState *machine)
{
    if (d->kernel) {
        ArmDomainBoot *boot = g_new0(ArmDomainBoot, 1);
        boot->domain = d;
        boot->info.ram_size = d->ram_size;
        boot->info.loader_start = d->ram_base;
        boot->info.board_id = -1;
        boot->info.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
        boot->info.get_dtb = arm_domain_dtb;
        boot->info.domain_memory = &d->root;
        char *kernel = machine->kernel_filename,
             *initrd = machine->initrd_filename;
        char *append = machine->kernel_cmdline;
        machine->kernel_filename = d->kernel;
        machine->initrd_filename = d->initrd;
        machine->kernel_cmdline = d->append;
        arm_load_kernel(ARM_CPU(d->cpus[0]), machine, &boot->info);
        machine->kernel_filename = kernel;
        machine->initrd_filename = initrd;
        machine->kernel_cmdline = append;
        d->opaque = boot;
    } else {
        if (d->firmware) {
            multiarch_virt_load_firmware(
                d, !strcmp(d->platform, "arm-virt") ? EM_AARCH64 : EM_ARM);
        }
        qemu_register_reset(arm_domain_reset, d);
    }
}
