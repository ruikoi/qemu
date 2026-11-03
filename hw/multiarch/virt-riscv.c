/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local RISC-V virt + MCU adapter, not intended for upstream contribution. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/riscv/virt.h"
#include "hw/multiarch/virt.h"

#define TYPE_HETERO_RISCV_VIRT MACHINE_TYPE_NAME("multiarch-virt-riscv")

typedef struct HeteroRISCVVirtState {
    RISCVVirtState parent_obj;
    HeteroVirtMCU *mcu;
} HeteroRISCVVirtState;

static void hetero_riscv_virt_init(MachineState *machine)
{
    HeteroRISCVVirtState *s = (HeteroRISCVVirtState *)machine;
    RISCVVirtState *vms = RISCV_VIRT_MACHINE(machine);
    MachineClass *parent = MACHINE_CLASS(
        object_class_get_parent(OBJECT_CLASS(MACHINE_GET_CLASS(machine))));

    if (vms->acpi != ON_OFF_AUTO_OFF) {
        error_report("hetero-virt-riscv requires acpi=off");
        exit(EXIT_FAILURE);
    }
    hetero_virt_prepare(machine, s->mcu, "rv64-riscv-cpu");
    parent->init(machine);
    hetero_virt_attach(machine, s->mcu, 0x12000000);
}

static void hetero_riscv_virt_instance_init(Object *obj)
{
    HeteroRISCVVirtState *s = (HeteroRISCVVirtState *)obj;
    RISCVVirtState *vms = RISCV_VIRT_MACHINE(obj);

    vms->acpi = ON_OFF_AUTO_OFF;
    vms->uart1_reserved = true;
    s->mcu = hetero_virt_mcu_new(MACHINE(obj));
}

static void hetero_riscv_virt_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->alias = "hetero-virt-riscv";
    mc->desc = "Local RISC-V virt main CPU with independent ARM/RISC-V MCU";
    mc->init = hetero_riscv_virt_init;
    mc->min_cpus = mc->max_cpus = mc->default_cpus = 1;
    mc->default_ram_size = 256 * MiB;
    mc->minimum_page_bits = 10;
    mc->cpu_index_to_instance_props = hetero_virt_cpu_index_to_props;
    mc->default_cpu_type = "rv64-riscv-cpu";
}

static const TypeInfo hetero_riscv_virt_type = {
    .name = TYPE_HETERO_RISCV_VIRT,
    .parent = TYPE_RISCV_VIRT_MACHINE,
    .instance_size = sizeof(HeteroRISCVVirtState),
    .instance_init = hetero_riscv_virt_instance_init,
    .class_init = hetero_riscv_virt_class_init,
};

static void hetero_riscv_virt_register_types(void)
{
    type_register_static(&hetero_riscv_virt_type);
}
type_init(hetero_riscv_virt_register_types)
