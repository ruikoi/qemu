/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/multiarch/domain.h"
#include "qemu/multiarch-runtime.h"
#include "cpu.h"

static CPUState *riscv_domain_cpu(Object *parent, MemoryRegion *memory,
                                  const MultiarchDomainConfig *config)
{
    CPUState *cpu = CPU(object_new("rv32-riscv-cpu"));

    object_property_add_child(parent, "cpu", OBJECT(cpu));
    object_property_set_link(OBJECT(cpu), "memory", OBJECT(memory),
                             &error_fatal);
    object_property_set_bool(OBJECT(cpu), "mmu", false, &error_fatal);
    object_property_set_bool(OBJECT(cpu), "s", false, &error_fatal);
    object_property_set_bool(OBJECT(cpu), "h", false, &error_fatal);
    object_property_set_int(OBJECT(cpu), "resetvec", config->resetvec,
                            &error_fatal);
    hetero_cpu_set_arch_id(cpu, config->id);
    qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    object_unref(OBJECT(cpu));
    return cpu;
}

static void riscv_domain_devices(CPUState *cpu, MemoryRegion *memory,
                                  const MultiarchDomainConfig *config)
{
    DeviceState *plic;
    char hart_config[] = "M";

    assert(RISCV_CPU(cpu)->env.mhartid == config->id);
    riscv_aclint_swi_create_with_memory(memory, config->swi, config->id, 1, false);
    riscv_aclint_mtimer_create_with_memory(memory, config->mtimer, 0x8000, config->id, 1,
                               config->id * 8, 0x7ff8, config->time_hz, true);
    plic = sifive_plic_create_with_memory(memory, config->plic, hart_config, 1,
                              config->id, config->num_irqs, 7,
                              0, 0x1000, 0x2000, 0x80,
                              0x200000, 0x1000, 0x400000);
    serial_mm_init(memory, config->uart, 0,
                   qdev_get_gpio_in(plic, 1), 115200,
                   config->serial, DEVICE_LITTLE_ENDIAN);
}

static const MultiarchCoreProfile riscv_mcu_profile = {
    .name = "rv32-mcu",
    .cpu_type = "rv32-riscv-cpu",
    .linux_m_mode = true,
    .create_cpu = riscv_domain_cpu,
    .create_devices = riscv_domain_devices,
};

static void riscv_domain_register(void)
{
    multiarch_core_profile_register(&riscv_mcu_profile);
}
type_init(riscv_domain_register)
