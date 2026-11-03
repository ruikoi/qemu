/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <libfdt.h>
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/multiarch/sg200x.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "system/address-spaces.h"
#include "target/arm/gtimer.h"
#include "cpu.h"

static CPUState *a53_create(Object *parent, MemoryRegion *memory,
                            const MultiarchDomainConfig *config)
{
    CPUState *cpu = CPU(object_new("cortex-a53-arm-cpu"));

    object_property_add_child(parent, "cpu", OBJECT(cpu));
    object_property_set_link(OBJECT(cpu), "memory", OBJECT(memory),
                             &error_fatal);
    object_property_set_bool(OBJECT(cpu), "has_el3", false, &error_fatal);
    object_property_set_int(OBJECT(cpu), "cntfrq", SG200X_TIME_HZ,
                            &error_fatal);
    qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    object_unref(OBJECT(cpu));
    return cpu;
}

static void a53_devices(CPUState *cpu, MemoryRegion *memory,
                        const MultiarchDomainConfig *config)
{
}

static const MultiarchCoreProfile a53_profile = {
    .name = "sg200x-a53",
    .cpu_type = "cortex-a53-arm-cpu",
    .create_cpu = a53_create,
    .create_devices = a53_devices,
};

static void register_a53(void)
{
    multiarch_core_profile_register(&a53_profile);
}
type_init(register_a53)

void sg200x_arm_intc(SG200XSoCState *s)
{
    DeviceState *gic = qdev_new("arm_gic");
    SysBusDevice *bus = SYS_BUS_DEVICE(gic);
    static const unsigned timer_irq[] = { 30, 27, 26, 29 };

    qdev_prop_set_uint32(gic, "revision", 2);
    qdev_prop_set_uint32(gic, "num-cpu", 1);
    qdev_prop_set_uint32(gic, "num-irq", 160);
    sysbus_realize_and_unref(bus, &error_fatal);
    sysbus_mmio_map(bus, 0, 0x01f01000);
    sysbus_mmio_map(bus, 1, 0x01f02000);
    sysbus_connect_irq(bus, 0,
                       qdev_get_gpio_in(DEVICE(s->main_cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(bus, 1,
                       qdev_get_gpio_in(DEVICE(s->main_cpu), ARM_CPU_FIQ));
    for (unsigned i = 0; i < ARRAY_SIZE(timer_irq); i++) {
        qdev_connect_gpio_out(DEVICE(s->main_cpu), i,
                              qdev_get_gpio_in(gic, 128 + timer_irq[i]));
    }
    s->main_intc = gic;
}

static void *get_dtb(const struct arm_boot_info *info, int *size)
{
    MachineState *machine = MACHINE(qdev_get_machine());

    *size = fdt_totalsize(machine->fdt);
    return g_memdup2(machine->fdt, *size);
}

void sg200x_arm_boot(SG200XSoCState *s, MachineState *machine)
{
    struct arm_boot_info *info = g_new0(struct arm_boot_info, 1);

    info->ram_size = s->ram_size - SG200X_RTOS_SIZE;
    info->loader_start = SG200X_DDR;
    info->board_id = -1;
    info->get_dtb = get_dtb;
    s->boot_info = info;
    /* Must run before the foreign ISA small core exists. */
    arm_load_kernel(ARM_CPU(s->main_cpu), machine, info);
}
