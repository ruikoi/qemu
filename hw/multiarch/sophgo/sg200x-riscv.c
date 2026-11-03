/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/multiarch-runtime.h"
#include "hw/multiarch/sg200x.h"
#include "hw/core/sysbus.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/riscv/boot.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "cpu.h"

static CPUState *c906_create(Object *parent, MemoryRegion *memory,
                             const MultiarchDomainConfig *config)
{
    CPUState *cpu = CPU(object_new("sg200x-c906-riscv-cpu"));

    object_property_add_child(parent, "cpu", OBJECT(cpu));
    object_property_set_link(OBJECT(cpu), "memory", OBJECT(memory),
                             &error_fatal);
    object_property_set_int(OBJECT(cpu), "resetvec", config->resetvec,
                            &error_fatal);
    /* QEMU feature initialization caches implied extensions by hart ID. */
    hetero_cpu_set_arch_id(cpu, config->id);
    qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    /* The actual SG200X domains independently expose local mhartid 0. */
    RISCV_CPU(cpu)->env.mhartid = 0;
    object_unref(OBJECT(cpu));
    return cpu;
}

static void c906_devices(CPUState *cpu, MemoryRegion *memory,
                         const MultiarchDomainConfig *config)
{
    /* Controller layout belongs to the SoC, not the C906 core profile. */
}

static const MultiarchCoreProfile c906_profile = {
    .name = "sg200x-c906",
    .cpu_type = "sg200x-c906-riscv-cpu",
    .create_cpu = c906_create,
    .create_devices = c906_devices,
};

static int64_t c906_arch_id(CPUState *cpu)
{
    /* QEMU controller lookup needs a globally unique ID, unlike mhartid. */
    return cpu->cpu_index;
}

static void c906_init(Object *obj)
{
    RISCV_CPU(obj)->cfg.sg200x_maee = true;
}

static void c906_class_init(ObjectClass *oc, const void *data)
{
    CPU_CLASS(oc)->get_arch_id = c906_arch_id;
}

static const TypeInfo c906_type = {
    .name = "sg200x-c906-riscv-cpu",
    .parent = TYPE_RISCV_CPU_THEAD_C906,
    .instance_init = c906_init,
    .class_init = c906_class_init,
};

static void register_c906(void)
{
    type_register_static(&c906_type);
    multiarch_core_profile_register(&c906_profile);
}
type_init(register_c906)

void sg200x_riscv_intc(SG200XSoCState *s, bool small)
{
    MemoryRegion *memory = small ? &s->small_root : get_system_memory();
    unsigned hart = small ? 1 : 0;
    DeviceState *plic;
    char main_config[] = "MS", small_config[] = "M";

    /* Both domains expose local hart 0 register offsets to their software. */
    riscv_aclint_swi_create_with_memory(memory, SG200X_CLINT, hart, 1, false);
    riscv_aclint_mtimer_create_with_memory(memory, SG200X_CLINT + 0x4000, 0xc000,
                               hart, 1, 0, 0x7ff8, SG200X_TIME_HZ, true);
    plic = sifive_plic_create_with_memory(memory, SG200X_PLIC,
                              small ? small_config : main_config,
                              1, hart, small ? 62 : 102, 7,
                              0, 0x1000, 0x2000, 0x80,
                              0x200000, 0x1000, 0x4000000);
    if (small) {
        s->small_intc = plic;
    } else {
        s->main_intc = plic;
    }
}

static void main_reset(void *opaque)
{
    cpu_reset(opaque);
}

void sg200x_riscv_boot(SG200XSoCState *s, MachineState *machine)
{
    RISCVHartArrayState harts = { .num_harts = 1,
                                 .harts = RISCV_CPU(s->main_cpu) };
    RISCVBootInfo info = { .ram_low_start = SG200X_DDR,
                           .ram_low_size = s->ram_size - SG200X_RTOS_SIZE };
    hwaddr start = SG200X_DDR;
    hwaddr end, kernel = 0, dtb;

    end = riscv_find_and_load_firmware(machine, &info, RISCV64_BIOS_BIN,
                                       &start, NULL);
    if (machine->kernel_filename) {
        kernel = riscv_calc_kernel_start_addr(&info, end);
        riscv_load_kernel(machine, &info, kernel, true, NULL);
        kernel = info.image_low_addr;
    }
    dtb = riscv_compute_fdt_addr(SG200X_DDR, info.ram_low_size, machine, &info);
    riscv_load_fdt(dtb, machine->fdt);
    riscv_setup_rom_reset_vec(machine, &harts, start, SG200X_ROM, 0x20000,
                               kernel, dtb);
    qemu_register_reset(main_reset, s->main_cpu);
}

void sg200x_riscv_fip_boot(SG200XSoCState *s, MachineState *machine,
                           hwaddr monitor, hwaddr next)
{
    RISCVHartArrayState harts = { .num_harts = 1,
                                 .harts = RISCV_CPU(s->main_cpu) };
    RISCVBootInfo info = { .ram_low_start = SG200X_DDR,
                           .ram_low_size = s->ram_size - SG200X_RTOS_SIZE };
    hwaddr dtb = riscv_compute_fdt_addr(SG200X_DDR, info.ram_low_size,
                                        machine, &info);

    riscv_load_fdt(dtb, machine->fdt);
    riscv_setup_rom_reset_vec(machine, &harts, monitor, SG200X_ROM,
                               0x20000, next, dtb);
    qemu_register_reset(main_reset, s->main_cpu);
}
