/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local ARM virt + MCU adapter, not intended for upstream contribution. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "hw/arm/virt.h"
#include "hw/multiarch/virt.h"

#define TYPE_HETERO_ARM_VIRT MACHINE_TYPE_NAME("multiarch-virt-arm")

typedef struct HeteroARMVirtState {
    VirtMachineState parent_obj;
    HeteroVirtMCU *mcu;
} HeteroARMVirtState;

static void hetero_arm_virt_init(MachineState *machine)
{
    HeteroARMVirtState *s = (HeteroARMVirtState *)machine;
    VirtMachineState *vms = VIRT_MACHINE(machine);
    MachineClass *parent = MACHINE_CLASS(
        object_class_get_parent(OBJECT_CLASS(MACHINE_GET_CLASS(machine))));

    if (vms->secure || vms->acpi != ON_OFF_AUTO_OFF) {
        error_report("hetero-virt-arm requires secure=off,acpi=off");
        exit(EXIT_FAILURE);
    }
    hetero_virt_prepare(machine, s->mcu, "cortex-a53-arm-cpu");
    parent->init(machine);
    hetero_virt_attach(machine, s->mcu, 0x60000000);
}

static void hetero_arm_virt_instance_init(Object *obj)
{
    HeteroARMVirtState *s = (HeteroARMVirtState *)obj;
    VirtMachineState *vms = VIRT_MACHINE(obj);

    vms->acpi = ON_OFF_AUTO_OFF;
    vms->uart1_reserved = true;
    s->mcu = hetero_virt_mcu_new(MACHINE(obj));
}

static void hetero_arm_virt_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->alias = "hetero-virt-arm";
    mc->desc = "Local ARM virt main CPU with independent ARM/RISC-V MCU";
    mc->init = hetero_arm_virt_init;
    mc->min_cpus = mc->max_cpus = mc->default_cpus = 1;
    mc->default_ram_size = 256 * MiB;
    mc->minimum_page_bits = 10;
    mc->cpu_index_to_instance_props = hetero_virt_cpu_index_to_props;
    mc->default_cpu_type = "cortex-a53-arm-cpu";
    mc->get_default_cpu_type = NULL;
    mc->get_valid_cpu_types = NULL;
}

static const TypeInfo hetero_arm_virt_type = {
    .name = TYPE_HETERO_ARM_VIRT,
    .parent = TYPE_VIRT_MACHINE,
    .instance_size = sizeof(HeteroARMVirtState),
    .instance_init = hetero_arm_virt_instance_init,
    .class_init = hetero_arm_virt_class_init,
};

static void hetero_arm_virt_register_types(void)
{
    type_register_static(&hetero_arm_virt_type);
}
type_init(hetero_arm_virt_register_types)
