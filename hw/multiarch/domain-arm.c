/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/arm/armv7m.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "hw/multiarch/domain.h"

static CPUState *arm_domain_cpu(Object *parent, MemoryRegion *memory,
                                const MultiarchDomainConfig *config)
{
    Object *container = object_new("armv7m");
    Clock *clock = clock_new(parent, "cpu-clock");
    CPUState *cpu;

    object_property_add_child(parent, "mcu", container);
    object_property_set_str(container, "cpu-type", "cortex-m3-arm-cpu",
                            &error_fatal);
    object_property_set_int(container, "num-irq", config->num_irqs,
                            &error_fatal);
    object_property_set_link(container, "memory", OBJECT(memory), &error_fatal);
    clock_set_hz(clock, config->clock_hz);
    qdev_connect_clock_in(DEVICE(container), "cpuclk", clock);
    sysbus_realize(SYS_BUS_DEVICE(container), &error_fatal);
    cpu = CPU(object_resolve_path_component(container, "cpu"));
    object_unref(container);
    return cpu;
}

static void arm_domain_devices(CPUState *cpu, MemoryRegion *memory,
                                const MultiarchDomainConfig *config)
{
    ARMv7MState *mcu = ARMV7M(OBJECT(cpu)->parent);

    serial_mm_init(memory, config->uart, 0,
                   qdev_get_gpio_in(DEVICE(mcu), 0), 115200,
                   config->serial, DEVICE_LITTLE_ENDIAN);
}

static const MultiarchCoreProfile arm_mcu_profile = {
    .name = "cortex-m3-arm-cpu",
    .cpu_type = "cortex-m3-arm-cpu",
    .vector_alias = true,
    .create_cpu = arm_domain_cpu,
    .create_devices = arm_domain_devices,
};

static void arm_domain_register(void)
{
    multiarch_core_profile_register(&arm_mcu_profile);
}
type_init(arm_domain_register)
