/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <libfdt.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/multiarch-runtime.h"
#include "hw/multiarch/sg200x.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/loader.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"

#define TYPE_DUO_MACHINE MACHINE_TYPE_NAME("milkv-duo")
OBJECT_DECLARE_SIMPLE_TYPE(DuoMachineState, DUO_MACHINE)
struct DuoMachineState {
    MachineState parent_obj;
    char *main_isa;
    char *rtos;
    bool rom_boot, fip_boot;
    char *fip;
    SG200XSoCState *soc;
};

static char *get_isa(Object *obj, Error **errp)
{
    return g_strdup(DUO_MACHINE(obj)->main_isa);
}

static void set_isa(Object *obj, const char *value, Error **errp)
{
    if (strcmp(value, "riscv64") && strcmp(value, "arm64")) {
        error_setg(errp, "main-isa must be riscv64 or arm64");
        return;
    }
    g_free(DUO_MACHINE(obj)->main_isa);
    DUO_MACHINE(obj)->main_isa = g_strdup(value);
}

static char *get_rtos(Object *obj, Error **errp)
{
    return g_strdup(DUO_MACHINE(obj)->rtos);
}

static void set_rtos(Object *obj, const char *value, Error **errp)
{
    g_free(DUO_MACHINE(obj)->rtos);
    DUO_MACHINE(obj)->rtos = g_strdup(value);
}

static char *get_boot_mode(Object *obj, Error **errp)
{
    DuoMachineState *s = DUO_MACHINE(obj);

    return g_strdup(s->fip_boot ? "fip" : s->rom_boot ? "rom" : "direct");
}

static void set_boot_mode(Object *obj, const char *value, Error **errp)
{
    if (strcmp(value, "direct") && strcmp(value, "rom") &&
        strcmp(value, "fip")) {
        error_setg(errp, "boot-mode must be direct, rom or fip");
        return;
    }
    DUO_MACHINE(obj)->rom_boot = !strcmp(value, "rom");
    DUO_MACHINE(obj)->fip_boot = !strcmp(value, "fip");
}

static char *get_fip(Object *obj, Error **errp)
{
    return g_strdup(DUO_MACHINE(obj)->fip);
}

static void set_fip(Object *obj, const char *value, Error **errp)
{
    g_free(DUO_MACHINE(obj)->fip);
    DUO_MACHINE(obj)->fip = g_strdup(value);
}

static const char *default_cpu(const MachineState *ms)
{
    return !strcmp(DUO_MACHINE(ms)->main_isa, "arm64") ?
           "cortex-a53-arm-cpu" : "sg200x-c906-riscv-cpu";
}

static CpuInstanceProperties cpu_props(MachineState *ms, unsigned index)
{
    return (CpuInstanceProperties) { .has_socket_id = true,
        .socket_id = index, .has_cluster_id = true, .cluster_id = index,
        .has_core_id = true, .core_id = 0,
        .has_thread_id = true, .thread_id = 0 };
}

static void add_reg(void *fdt, const char *node, hwaddr base, hwaddr size)
{
    qemu_fdt_add_subnode(fdt, node);
    qemu_fdt_setprop_sized_cells(fdt, node, "reg", 2, base, 2, size);
}

static void peripheral_irq(void *fdt, const char *node, bool arm,
                           uint32_t parent, unsigned riscv_irq,
                           unsigned arm_spi)
{
    qemu_fdt_setprop_cell(fdt, node, "interrupt-parent", parent);
    if (arm) {
        qemu_fdt_setprop_cells(fdt, node, "interrupts", 0, arm_spi, 4);
    } else {
        qemu_fdt_setprop_cell(fdt, node, "interrupts", riscv_irq);
    }
}

static void peripheral_dtb(void *fdt, bool arm, uint32_t intc)
{
    unsigned i;

    qemu_fdt_add_subnode(fdt, "/peripheral-clock");
    qemu_fdt_setprop_string(fdt, "/peripheral-clock", "compatible",
                           "fixed-clock");
    qemu_fdt_setprop_cell(fdt, "/peripheral-clock", "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/peripheral-clock", "clock-frequency",
                         SG200X_TIME_HZ);
    qemu_fdt_setprop_cell(fdt, "/peripheral-clock", "phandle", 3);
    for (i = 0; i < 8; i++) {
        g_autofree char *node = g_strdup_printf("/timer@%x",
                                               0x030a0000 + i * 0x14);

        add_reg(fdt, node, 0x030a0000 + i * 0x14, 0x14);
        qemu_fdt_setprop_string(fdt, node, "compatible", "snps,dw-apb-timer");
        qemu_fdt_setprop_cell(fdt, node, "clocks", 3);
        qemu_fdt_setprop_string(fdt, node, "clock-names", "timer");
        peripheral_irq(fdt, node, arm, intc, 79 + i, 63 + i);
    }
    for (i = 0; i < 4; i++) {
        g_autofree char *node = g_strdup_printf("/gpio@%x",
                                               0x03020000 + i * 0x1000);
        g_autofree char *port = g_strdup_printf("%s/gpio-port@0", node);

        add_reg(fdt, node, 0x03020000 + i * 0x1000, 0x100);
        qemu_fdt_setprop_string(fdt, node, "compatible", "snps,dw-apb-gpio");
        qemu_fdt_setprop_cell(fdt, node, "#address-cells", 1);
        qemu_fdt_setprop_cell(fdt, node, "#size-cells", 0);
        qemu_fdt_setprop_cell(fdt, node, "clocks", 3);
        qemu_fdt_setprop_string(fdt, node, "clock-names", "bus");
        qemu_fdt_add_subnode(fdt, port);
        qemu_fdt_setprop_string(fdt, port, "compatible",
                               "snps,dw-apb-gpio-port");
        qemu_fdt_setprop_cell(fdt, port, "reg", 0);
        qemu_fdt_setprop(fdt, port, "gpio-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, port, "#gpio-cells", 2);
        qemu_fdt_setprop_cell(fdt, port, "snps,nr-gpios", 32);
        qemu_fdt_setprop(fdt, port, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, port, "#interrupt-cells", 2);
        peripheral_irq(fdt, port, arm, intc, 60 + i, 44 + i);
    }
    for (i = 0; i < 4; i++) {
        g_autofree char *node = g_strdup_printf("/spi@%x",
                                               0x04180000 + i * 0x10000);

        add_reg(fdt, node, 0x04180000 + i * 0x10000, 0x1000);
        qemu_fdt_setprop_string(fdt, node, "compatible", "snps,dw-apb-ssi");
        qemu_fdt_setprop_cell(fdt, node, "#address-cells", 1);
        qemu_fdt_setprop_cell(fdt, node, "#size-cells", 0);
        qemu_fdt_setprop_cell(fdt, node, "num-cs", 1);
        qemu_fdt_setprop_cell(fdt, node, "clocks", 3);
        qemu_fdt_setprop_string(fdt, node, "clock-names", "ssi_clk");
        peripheral_irq(fdt, node, arm, intc, 54 + i, 38 + i);
    }
}

static void create_dtb(DuoMachineState *s)
{
    MachineState *ms = MACHINE(s);
    SG200XSoCState *soc = s->soc;
    bool arm = soc->arm;
    void *fdt;
    int size;
    const char *plic = "/interrupt-controller@70000000";
    const char *gic = "/interrupt-controller@1f01000";
    uint32_t cpu_intc = 1, intc = 2;
    uint64_t usable = ms->ram_size - SG200X_RTOS_SIZE;
    g_autofree char *reserved = g_strdup_printf(
        "/reserved-memory/rtos@%" PRIx64, SG200X_DDR + usable);

    ms->fdt = fdt = create_device_tree(&size);
    if (!fdt) {
        error_report("cannot allocate SG200X device tree");
        exit(EXIT_FAILURE);
    }
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);
    qemu_fdt_setprop_string(fdt, "/", "model", MACHINE_GET_CLASS(ms)->desc);
    qemu_fdt_setprop_string(fdt, "/", "compatible", "qemu,sg200x-directboot");
    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "/serial@4140000");
    if (ms->kernel_cmdline) {
        qemu_fdt_setprop_string(fdt, "/chosen", "bootargs", ms->kernel_cmdline);
    }
    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial0", "/serial@4140000");
    add_reg(fdt, "/memory@80000000", SG200X_DDR, usable);
    qemu_fdt_setprop_string(fdt, "/memory@80000000", "device_type", "memory");
    qemu_fdt_add_subnode(fdt, "/reserved-memory");
    qemu_fdt_setprop_cell(fdt, "/reserved-memory", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/reserved-memory", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/reserved-memory", "ranges", NULL, 0);
    add_reg(fdt, reserved, SG200X_DDR + usable, SG200X_RTOS_SIZE);
    qemu_fdt_setprop(fdt, reserved, "no-map", NULL, 0);
    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", arm ? 2 : 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency", SG200X_TIME_HZ);
    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "device_type", "cpu");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "compatible",
                            arm ? "arm,cortex-a53" : "thead,c906");
    if (!arm) {
        const char compatible[] = "thead,c906\0riscv";

        qemu_fdt_setprop(fdt, "/cpus/cpu@0", "compatible",
                         compatible, sizeof(compatible));
    }
    if (arm) {
        qemu_fdt_setprop_cells(fdt, "/cpus/cpu@0", "reg", 0, 0);
        qemu_fdt_add_subnode(fdt, gic);
        qemu_fdt_setprop_string(fdt, gic, "compatible", "arm,cortex-a15-gic");
        qemu_fdt_setprop_sized_cells(fdt, gic, "reg", 2, 0x01f01000, 2, 0x1000,
                                    2, 0x01f02000, 2, 0x2000);
        qemu_fdt_setprop_cell(fdt, gic, "#interrupt-cells", 3);
        qemu_fdt_setprop(fdt, gic, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, gic, "phandle", intc);
        qemu_fdt_add_subnode(fdt, "/timer");
        qemu_fdt_setprop_string(fdt, "/timer", "compatible", "arm,armv8-timer");
        qemu_fdt_setprop_cell(fdt, "/timer", "interrupt-parent", intc);
        qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                                1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4);
    } else {
        const char extensions[] = "i\0m\0a\0f\0d\0c\0zicntr\0zicsr\0"
                                  "zifencei\0zihpm";

        qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "riscv,isa-base", "rv64i");
        qemu_fdt_setprop(fdt, "/cpus/cpu@0", "riscv,isa-extensions",
                         extensions, sizeof(extensions));
        qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0", "reg", 0);
        qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "riscv,isa", "rv64imafdc");
        qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "mmu-type", "riscv,sv39");
        qemu_fdt_add_subnode(fdt, "/cpus/cpu@0/interrupt-controller");
        qemu_fdt_setprop_string(fdt, "/cpus/cpu@0/interrupt-controller",
                                "compatible", "riscv,cpu-intc");
        qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0/interrupt-controller",
                              "#interrupt-cells", 1);
        qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0/interrupt-controller",
                              "phandle", cpu_intc);
        qemu_fdt_setprop(fdt, "/cpus/cpu@0/interrupt-controller",
                         "interrupt-controller", NULL, 0);
        add_reg(fdt, plic, SG200X_PLIC, 0x4000000);
        /* Generic PLIC binding for direct boot; no vendor control emulation. */
        qemu_fdt_setprop_string(fdt, plic, "compatible", "riscv,plic0");
        qemu_fdt_setprop_cell(fdt, plic, "#interrupt-cells", 1);
        qemu_fdt_setprop(fdt, plic, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, plic, "phandle", intc);
        qemu_fdt_setprop_cell(fdt, plic, "riscv,ndev", 101);
        qemu_fdt_setprop_cells(fdt, plic, "interrupts-extended",
                                cpu_intc, 11, cpu_intc, 9);
        add_reg(fdt, "/clint@74000000", SG200X_CLINT, 0x10000);
        qemu_fdt_setprop_string(fdt, "/clint@74000000", "compatible",
                                "riscv,clint0");
        qemu_fdt_setprop_cells(fdt, "/clint@74000000", "interrupts-extended",
                                cpu_intc, 3, cpu_intc, 7);
    }
    add_reg(fdt, "/serial@4140000", SG200X_UART0, 0x10000);
    qemu_fdt_setprop_string(fdt, "/serial@4140000", "compatible", "ns16550a");
    qemu_fdt_setprop_cell(fdt, "/serial@4140000", "reg-shift", 2);
    qemu_fdt_setprop_cell(fdt, "/serial@4140000", "reg-io-width", 4);
    qemu_fdt_setprop_cell(fdt, "/serial@4140000", "clock-frequency",
                          SG200X_TIME_HZ);
    qemu_fdt_setprop_cell(fdt, "/serial@4140000", "interrupt-parent", intc);
    if (arm) {
        qemu_fdt_setprop_cells(fdt, "/serial@4140000", "interrupts", 0, 28, 4);
    } else {
        qemu_fdt_setprop_cell(fdt, "/serial@4140000", "interrupts", 44);
    }
    peripheral_dtb(fdt, arm, intc);
    /* UART1 and hart1 belong to RTOS; Linux must not claim them. */
}

static void board_init(MachineState *ms)
{
    DuoMachineState *s = DUO_MACHINE(ms);
    bool arm = !strcmp(s->main_isa, "arm64");
    const char *types[] = { default_cpu(ms), "sg200x-c906-riscv-cpu" };
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    DeviceState *dev;
    bool cv1800 = !strcmp(mc->name, "milkv-duo");
    hwaddr rom_base = cv1800 ? SG200X_ROM : 0x04418000;
    size_t rom_size = cv1800 ? 0x10000 : 0x18000;

    if ((s->fip && !s->fip_boot) || (s->fip_boot &&
        (arm || cv1800 || !s->fip || ms->kernel_filename ||
         ms->initrd_filename ||
         (ms->firmware && strcmp(ms->firmware, "none"))))) {
        error_report("FIP boot requires SG200X RISC-V and fip=FILE, without "
                     "-bios, -kernel or -initrd overrides");
        exit(EXIT_FAILURE);
    }
    if (s->rom_boot && (arm || s->rtos || ms->kernel_filename ||
                        ms->initrd_filename || !ms->firmware ||
                        !strcmp(ms->firmware, "none"))) {
        error_report("ROM boot requires RISC-V, a raw -bios image and no "
                     "-kernel, -initrd or rtos override");
        exit(EXIT_FAILURE);
    }
    if (arm && !strcmp(mc->name, "milkv-duo")) {
        error_report("CV1800B Milk-V Duo has no Cortex-A53 main core");
        exit(EXIT_FAILURE);
    }
    if (ms->ram_size != mc->default_ram_size || ms->dtb ||
        strcmp(ms->cpu_type, types[0])) {
        error_report("SG200X requires board DDR size, generated DTB and "
                     "its selected C906/A53 CPU model");
        exit(EXIT_FAILURE);
    }
    multiarch_prepare_cpu_types(types, ARRAY_SIZE(types), &error_fatal);
    memory_region_add_subregion(get_system_memory(), SG200X_DDR, ms->ram);
    dev = qdev_new(TYPE_SG200X_SOC);
    s->soc = SG200X_SOC(dev);
    object_property_add_child(OBJECT(ms), "soc", OBJECT(dev));
    qdev_prop_set_bit(dev, "arm-main", arm);
    qdev_prop_set_bit(dev, "rom-boot", s->rom_boot);
    qdev_prop_set_uint64(dev, "rom-base", rom_base);
    qdev_prop_set_uint64(dev, "rom-size", rom_size);
    qdev_prop_set_uint64(dev, "tpu-size", cv1800 ? 0x10000 : 0x40000);
    qdev_prop_set_uint64(dev, "ram-size", ms->ram_size);
    if (s->rtos) {
        qdev_prop_set_string(dev, "rtos", s->rtos);
    }
    qdev_realize_and_unref(dev, NULL, &error_fatal);
    if (s->rom_boot) {
        if (load_image_targphys(ms->firmware, rom_base, rom_size,
                                &error_fatal) <= 0) {
            error_report("cannot load raw boot ROM %s (maximum %zu bytes)",
                         ms->firmware, rom_size);
            exit(EXIT_FAILURE);
        }
        sg200x_soc_attach_small(s->soc);
        return;
    }
    create_dtb(s);
    if (s->fip_boot) {
        hwaddr monitor, next;

        sg200x_fip_load(s->soc, s->fip, &monitor, &next);
        sg200x_riscv_fip_boot(s->soc, ms, monitor, next);
    } else if (arm) {
        sg200x_arm_boot(s->soc, ms);
    } else {
        sg200x_riscv_boot(s->soc, ms);
    }
    sg200x_soc_attach_small(s->soc);
}

static void board_instance_init(Object *obj)
{
    DUO_MACHINE(obj)->main_isa = g_strdup("riscv64");
}

static void board_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Milk-V Duo (CV1800B), experimental direct boot";
    mc->init = board_init;
    mc->min_cpus = mc->max_cpus = mc->default_cpus = 1;
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "sg200x.ddr";
    mc->default_cpu_type = "sg200x-c906-riscv-cpu";
    mc->get_default_cpu_type = default_cpu;
    mc->cpu_index_to_instance_props = cpu_props;
    mc->minimum_page_bits = 10;
    object_class_property_add_str(oc, "main-isa", get_isa, set_isa);
    object_class_property_add_str(oc, "boot-mode", get_boot_mode,
                                  set_boot_mode);
    object_class_property_set_description(oc, "boot-mode",
        "direct Linux boot, raw RISC-V ROM or preinitialized FIP boot");
    object_class_property_add_str(oc, "fip", get_fip, set_fip);
    object_class_property_set_description(oc, "fip",
        "Unsigned CVBL01/CVLD02 FIP for boot-mode=fip");
    object_class_property_add_str(oc, "rtos", get_rtos, set_rtos);
    object_class_property_set_description(oc, "rtos",
        "RV64 ELF in TPU SRAM or reserved top 8 MiB of DDR");
}

static void duo256_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Milk-V Duo256M (SG2002), experimental direct boot";
    mc->default_ram_size = 256 * MiB;
}

static void duos_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Milk-V Duo S (SG2000), experimental direct boot";
    mc->default_ram_size = 512 * MiB;
}

static const TypeInfo board_types[] = {
    { .name = TYPE_DUO_MACHINE, .parent = TYPE_MACHINE,
      .instance_size = sizeof(DuoMachineState),
      .instance_init = board_instance_init, .class_init = board_class_init },
    { .name = MACHINE_TYPE_NAME("milkv-duo256m"), .parent = TYPE_DUO_MACHINE,
      .class_init = duo256_class_init },
    { .name = MACHINE_TYPE_NAME("milkv-duos"), .parent = TYPE_DUO_MACHINE,
      .class_init = duos_class_init },
};
DEFINE_TYPES(board_types)
