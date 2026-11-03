/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local virt + independent MCU experiment, not an upstream contribution. */
#include "qemu/osdep.h"
#include "elf.h"
#include <libfdt.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/multiarch-runtime.h"
#include "hw/multiarch/virt.h"
#include "hw/multiarch/domain.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/sysbus.h"
#include "hw/cpu/cluster.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "system/tcg.h"
#include "system/system.h"
#include "migration/blocker.h"

struct HeteroVirtMCU {
    DeviceState parent_obj;
    char *profile;
    char *firmware;
    char *kernel;
    char *append;
    uint64_t ram_size;
    uint64_t boot_pc;
    bool arm_mcu;
    CPUState *cpu;
    hwaddr shared_addr;
    uint64_t entry;
    MemoryRegion root;
    MemoryRegion ram;
    MemoryRegion vectors;
    MemoryRegion shared;
    MemoryRegion shared_view;
    const MultiarchCoreProfile *core_profile;
    Error *migration_blocker;
};

HeteroVirtMCU *hetero_virt_mcu_new(MachineState *machine)
{
    HeteroVirtMCU *s = HETERO_VIRT_MCU(qdev_new(TYPE_HETERO_VIRT_MCU));

    object_property_add_child(OBJECT(machine), "mcu-subsystem", OBJECT(s));
    object_property_add_alias(OBJECT(machine), "mcu", OBJECT(s), "cpu-profile");
    object_property_add_alias(OBJECT(machine), "mcu-firmware", OBJECT(s),
                              "firmware");
    object_property_set_description(OBJECT(machine), "mcu",
                                    "Independent cortex-m3-arm-cpu or rv32-mcu");
    object_property_set_description(OBJECT(machine), "mcu-firmware",
                                    "MCU ELF32 executable or raw RAM image");
    object_property_add_alias(OBJECT(machine), "mcu-ram", OBJECT(s),
                              "ram-size");
    object_property_add_alias(OBJECT(machine), "mcu-kernel", OBJECT(s),
                              "kernel");
    object_property_add_alias(OBJECT(machine), "mcu-append", OBJECT(s),
                              "append");
    object_unref(OBJECT(s));
    return s;
}

void hetero_virt_prepare(MachineState *machine, HeteroVirtMCU *s,
                         const char *main_cpu_type)
{
    const char *mcu_type;
    const char *types[2];

    if (!tcg_enabled()) {
        error_report("hetero-virt requires the TCG accelerator");
        exit(EXIT_FAILURE);
    }
    if (machine->smp.cpus != 1 || machine->smp.max_cpus != 1) {
        error_report("hetero-virt currently supports one main CPU plus one MCU");
        exit(EXIT_FAILURE);
    }
    if (strcmp(machine->cpu_type, main_cpu_type)) {
        error_report("this hetero-virt machine requires main CPU type %s",
                     main_cpu_type);
        exit(EXIT_FAILURE);
    }
    if (machine->dtb) {
        error_report("hetero-virt currently requires its generated device tree");
        exit(EXIT_FAILURE);
    }
    if (machine->ram_size < 64 * MiB || machine->ram_size > 512 * MiB) {
        error_report("hetero-virt main RAM must be between 64 and 512 MiB");
        exit(EXIT_FAILURE);
    }
    s->core_profile = multiarch_core_profile_find(s->profile);
    if (!s->core_profile) {
        error_report("unsupported hetero-virt MCU profile: %s", s->profile);
        exit(EXIT_FAILURE);
    }
    s->arm_mcu = s->core_profile->vector_alias;
    mcu_type = s->core_profile->cpu_type;
    if (s->ram_size < MiB || s->ram_size > 128 * MiB ||
        (s->ram_size & (MiB - 1))) {
        error_report("MCU RAM must be 1..128 MiB, in whole MiB units");
        exit(EXIT_FAILURE);
    }
    if (s->kernel && (!s->core_profile->linux_m_mode || s->firmware ||
                      s->ram_size < 16 * MiB)) {
        error_report("mcu-kernel requires rv32-mcu, at least 16 MiB RAM, "
                     "and no mcu-firmware");
        exit(EXIT_FAILURE);
    }
    if (s->append && !s->kernel) {
        error_report("mcu-append requires mcu-kernel");
        exit(EXIT_FAILURE);
    }
    types[0] = main_cpu_type;
    types[1] = mcu_type;
    multiarch_prepare_cpu_types(types, ARRAY_SIZE(types), &error_fatal);
    error_setg(&s->migration_blocker,
               "hetero-virt composition does not support migration");
    migrate_add_blocker(&s->migration_blocker, &error_fatal);
}

CpuInstanceProperties hetero_virt_cpu_index_to_props(MachineState *machine,
                                                     unsigned cpu_index)
{
    MachineClass *parent = MACHINE_CLASS(
        object_class_get_parent(OBJECT_CLASS(MACHINE_GET_CLASS(machine))));

    if (cpu_index == 0) {
        return parent->cpu_index_to_instance_props(machine, cpu_index);
    }
    assert(cpu_index == 1);
    /* MCU is a separate domain, not another slot in the Linux SMP topology. */
    return (CpuInstanceProperties) {
        .has_socket_id = true, .socket_id = 1,
        .has_cluster_id = true, .cluster_id = 1,
        .has_core_id = true, .core_id = 0,
        .has_thread_id = true, .thread_id = 0,
    };
}

static bool mcu_ram_contains(HeteroVirtMCU *s, uint64_t addr, uint64_t size)
{
    uint64_t usable = s->ram_size - (s->kernel ? 2 * MiB : 0);

    if (size > usable) {
        return false;
    }
    if (addr >= HETERO_MCU_RAM_BASE &&
        addr - HETERO_MCU_RAM_BASE <= usable - size) {
        return true;
    }
    return s->arm_mcu && addr <= usable - size;
}

static void mcu_load_firmware(HeteroVirtMCU *s)
{
    g_autofree char *data = NULL;
    g_autoptr(GError) error = NULL;
    size_t length;
    uint64_t entry;
    unsigned elf_machine = s->arm_mcu ? EM_ARM : EM_RISCV;
    const char *filename = s->kernel ? s->kernel : s->firmware;
    uint64_t usable = s->ram_size - (s->kernel ? 2 * MiB : 0);

    s->entry = HETERO_MCU_RAM_BASE;
    if (!filename) {
        return;
    }
    if (!g_file_get_contents(filename, &data, &length, &error)) {
        error_report("cannot read MCU firmware: %s", error->message);
        exit(EXIT_FAILURE);
    }
    if (length >= SELFMAG && !memcmp(data, ELFMAG, SELFMAG)) {
        Elf32_Ehdr ehdr;
        uint32_t phoff;
        uint16_t phnum;
        bool loaded = false;

        if (length < sizeof(ehdr)) {
            goto invalid_elf;
        }
        memcpy(&ehdr, data, sizeof(ehdr));
        phoff = le32_to_cpu(ehdr.e_phoff);
        phnum = le16_to_cpu(ehdr.e_phnum);
        entry = le32_to_cpu(ehdr.e_entry);
        if (ehdr.e_ident[EI_CLASS] != ELFCLASS32 ||
            ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
            le16_to_cpu(ehdr.e_machine) != elf_machine ||
            le16_to_cpu(ehdr.e_type) != ET_EXEC ||
            le16_to_cpu(ehdr.e_phentsize) != sizeof(Elf32_Phdr) ||
            phoff > length || phnum > (length - phoff) / sizeof(Elf32_Phdr) ||
            (!s->arm_mcu && (entry & 1)) ||
            !mcu_ram_contains(s, entry & ~1ULL, 1)) {
            goto invalid_elf;
        }
        for (unsigned i = 0; i < phnum; i++) {
            Elf32_Phdr phdr;
            uint32_t filesz, memsz, offset;

            memcpy(&phdr, data + phoff + i * sizeof(phdr), sizeof(phdr));
            if (le32_to_cpu(phdr.p_type) != PT_LOAD) {
                continue;
            }
            filesz = le32_to_cpu(phdr.p_filesz);
            memsz = le32_to_cpu(phdr.p_memsz);
            offset = le32_to_cpu(phdr.p_offset);
            if (filesz > memsz || offset > length || filesz > length - offset ||
                !mcu_ram_contains(s, le32_to_cpu(phdr.p_paddr), memsz)) {
                goto invalid_elf;
            }
            loaded |= memsz != 0;
        }
        if (!loaded || load_elf_as(filename, NULL, NULL, NULL, &entry,
                                  NULL, NULL, NULL, ELFDATA2LSB, elf_machine,
                                  0, 0, s->cpu->as) < 0) {
            goto invalid_elf;
        }
        s->entry = entry;
    } else {
        if (!length || length > usable ||
            load_image_targphys_as(filename, HETERO_MCU_RAM_BASE,
                                  usable, s->cpu->as,
                                  &error_fatal) < 0) {
            error_report("raw MCU image must fit in available MCU private RAM");
            exit(EXIT_FAILURE);
        }
    }
    return;

invalid_elf:
    error_report("MCU firmware must be a matching little-endian ELF32 executable"
                 " with entry and all PT_LOAD segments in MCU private RAM");
    exit(EXIT_FAILURE);
}

/* Linux M-mode boot ABI: a0 = hart ID, a1 = private DTB address. */
static void mcu_linux_boot(HeteroVirtMCU *s)
{
    const char *cpu_intc = "/cpus/cpu@1/interrupt-controller";
    static const char extensions[] =
        "i\0m\0a\0f\0d\0c\0zicsr\0zifencei";
    int size;
    g_autofree void *fdt = create_device_tree(&size);
    hwaddr dtb = HETERO_MCU_RAM_BASE + s->ram_size - MiB;
    hwaddr boot = dtb - MiB;
    uint32_t reset_vec[] = {
        0x00100513,                           /* addi a0, zero, 1 */
        ((dtb + 0x800) & 0xfffff000) | 0x5b7, /* lui a1, dtb_hi */
        ((dtb & 0xfff) << 20) | 0x58593,      /* addi a1, a1, dtb_lo */
        ((s->entry + 0x800) & 0xfffff000) | 0x2b7,
        ((s->entry & 0xfff) << 20) | 0x28293, /* addi t0, t0, entry_lo */
        0x00028067,                           /* jalr zero, t0, 0 */
    };

    if (!fdt) {
        error_report("cannot create MCU Linux device tree");
        exit(EXIT_FAILURE);
    }
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 1);
    qemu_fdt_setprop_string(fdt, "/", "compatible", "qemu,hetero-rv32-mcu");
    qemu_fdt_setprop_string(fdt, "/", "model",
                            "Independent RV32 no-MMU domain");
    qemu_fdt_add_subnode(fdt, "/memory@10000000");
    qemu_fdt_setprop_string(fdt, "/memory@10000000", "device_type", "memory");
    qemu_fdt_setprop_cells(fdt, "/memory@10000000", "reg",
                           HETERO_MCU_RAM_BASE, s->ram_size);
    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
                          HETERO_RISCV_TIME_HZ);
    qemu_fdt_add_subnode(fdt, "/cpus/cpu@1");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@1", "device_type", "cpu");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@1", "compatible", "riscv");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@1", "status", "okay");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@1", "reg", 1);
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@1", "riscv,isa",
                            "rv32imafdc_zicsr_zifencei");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@1", "riscv,isa-base", "rv32i");
    qemu_fdt_setprop(fdt, "/cpus/cpu@1", "riscv,isa-extensions",
                     extensions, sizeof(extensions));
    qemu_fdt_add_subnode(fdt, cpu_intc);
    qemu_fdt_setprop_string(fdt, cpu_intc, "compatible", "riscv,cpu-intc");
    qemu_fdt_setprop_cell(fdt, cpu_intc, "#interrupt-cells", 1);
    qemu_fdt_setprop(fdt, cpu_intc, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, cpu_intc, "phandle", 1);
    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 1);
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_add_subnode(fdt, "/soc/clint@2000000");
    qemu_fdt_setprop_string(fdt, "/soc/clint@2000000", "compatible",
                            "riscv,clint0");
    qemu_fdt_setprop_cells(fdt, "/soc/clint@2000000", "reg", 0x02000000,
                           0x10000);
    qemu_fdt_setprop_cells(fdt, "/soc/clint@2000000", "interrupts-extended", 1,
                           3, 1, 7);
    qemu_fdt_add_subnode(fdt, "/soc/plic@c000000");
    qemu_fdt_setprop_string(fdt, "/soc/plic@c000000", "compatible",
                            "riscv,plic0");
    qemu_fdt_setprop_cells(fdt, "/soc/plic@c000000", "reg", 0x0c000000,
                           0x400000);
    qemu_fdt_setprop_cell(fdt, "/soc/plic@c000000", "#interrupt-cells", 1);
    qemu_fdt_setprop(fdt, "/soc/plic@c000000", "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, "/soc/plic@c000000", "riscv,ndev", 31);
    qemu_fdt_setprop_cell(fdt, "/soc/plic@c000000", "phandle", 2);
    qemu_fdt_setprop_cells(fdt, "/soc/plic@c000000", "interrupts-extended", 1,
                           11);
    qemu_fdt_add_subnode(fdt, "/soc/serial@40000000");
    qemu_fdt_setprop_string(fdt, "/soc/serial@40000000", "compatible",
                            "ns16550a");
    qemu_fdt_setprop_cells(fdt, "/soc/serial@40000000", "reg", HETERO_MCU_UART,
                           8);
    qemu_fdt_setprop_cell(fdt, "/soc/serial@40000000", "clock-frequency",
                          1843200);
    qemu_fdt_setprop_cell(fdt, "/soc/serial@40000000", "interrupt-parent", 2);
    qemu_fdt_setprop_cell(fdt, "/soc/serial@40000000", "interrupts", 1);
    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path",
                            "/soc/serial@40000000");
    qemu_fdt_setprop_string(fdt, "/chosen", "bootargs", s->append ? s->append :
                           "console=ttyS0,115200 "
                           "earlycon=uart8250,mmio,0x40000000 "
                           "rdinit=/init");
    if (fdt_add_mem_rsv(fdt, boot, 2 * MiB) || fdt_pack(fdt) ||
        fdt_totalsize(fdt) > MiB) {
        error_report("MCU Linux device tree does not fit its reserved RAM");
        exit(EXIT_FAILURE);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(reset_vec); i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }
    rom_add_blob_fixed_as("mcu-linux-dtb", fdt, fdt_totalsize(fdt), dtb,
                          s->cpu->as);
    rom_add_blob_fixed_as("mcu-linux-reset", reset_vec, sizeof(reset_vec),
                          boot, s->cpu->as);
    s->boot_pc = boot;
}

static void mcu_reset(void *opaque)
{
    HeteroVirtMCU *s = opaque;

    cpu_reset(s->cpu);
    if (!s->arm_mcu) {
        cpu_set_pc(s->cpu, s->kernel ? s->boot_pc : s->entry);
    }
    /* Permit hardware-only configuration before a guest image is selected. */
    if (!s->firmware && !s->kernel) {
        s->cpu->halted = 1;
    }
}

static void mcu_realize(DeviceState *dev, Error **errp)
{
    HeteroVirtMCU *s = HETERO_VIRT_MCU(dev);
    const MultiarchDomainConfig config = {
        .id = 1,
        .num_irqs = 32,
        .resetvec = HETERO_MCU_RAM_BASE,
        .uart = HETERO_MCU_UART,
        .swi = 0x02000000,
        .mtimer = 0x02004000,
        .plic = 0x0c000000,
        .clock_hz = HETERO_MCU_CLOCK_HZ,
        .time_hz = HETERO_RISCV_TIME_HZ,
        .serial = serial_hd(1),
    };

    memory_region_init(&s->root, OBJECT(s), "mcu-memory", UINT64_MAX);
    memory_region_init_ram_flags_nomigrate(&s->ram, OBJECT(s), "mcu.private",
                                         s->ram_size, 0, &error_fatal);
    memory_region_add_subregion(&s->root, HETERO_MCU_RAM_BASE, &s->ram);
    memory_region_init_ram_flags_nomigrate(&s->shared, OBJECT(s), "mcu.shared",
                                         HETERO_SHARED_SIZE, 0, &error_fatal);
    memory_region_add_subregion(get_system_memory(), s->shared_addr, &s->shared);
    memory_region_init_alias(&s->shared_view, OBJECT(s), "mcu.shared-view",
                             &s->shared, 0, HETERO_SHARED_SIZE);
    memory_region_add_subregion(&s->root, HETERO_MCU_SHARED, &s->shared_view);
    if (s->arm_mcu) {
        memory_region_init_alias(&s->vectors, OBJECT(s), "mcu.vectors",
                                 &s->ram, 0, s->ram_size);
        memory_region_add_subregion(&s->root, 0, &s->vectors);
    }
    s->cpu = multiarch_domain_create(OBJECT(s), &s->root,
                                     s->core_profile, &config);
    multiarch_domain_create_devices(s->cpu, &s->root,
                                     s->core_profile, &config);
    mcu_load_firmware(s);
    if (s->kernel) {
        mcu_linux_boot(s);
    }
    qemu_register_reset(mcu_reset, s);
}

void hetero_virt_attach(MachineState *machine, HeteroVirtMCU *s,
                        hwaddr shared_addr)
{
    g_autofree char *node = NULL;
    const uint32_t phandle = qemu_fdt_alloc_phandle(machine->fdt);

    /* Parent virt has finished CPU enumeration and boot/reset registration. */
    assert(first_cpu && !CPU_NEXT(first_cpu));
    first_cpu->cluster_index = 0;
    tcg_update_cflags(first_cpu);
    s->shared_addr = shared_addr;
    qdev_realize(DEVICE(s), NULL, &error_fatal);

    qemu_fdt_add_subnode(machine->fdt, "/reserved-memory");
    qemu_fdt_setprop_cell(machine->fdt, "/reserved-memory", "#address-cells",
                          2);
    qemu_fdt_setprop_cell(machine->fdt, "/reserved-memory", "#size-cells", 2);
    qemu_fdt_setprop(machine->fdt, "/reserved-memory", "ranges", NULL, 0);
    node = g_strdup_printf("/reserved-memory/hetero-shared@%" HWADDR_PRIx,
                           shared_addr);
    qemu_fdt_add_subnode(machine->fdt, node);
    qemu_fdt_setprop_string(machine->fdt, node, "compatible",
                            "shared-dma-pool");
    qemu_fdt_setprop_sized_cells(machine->fdt, node, "reg", 2, shared_addr,
                               2, HETERO_SHARED_SIZE);
    qemu_fdt_setprop(machine->fdt, node, "no-map", NULL, 0);
    qemu_fdt_setprop_cell(machine->fdt, node, "phandle", phandle);
    qemu_fdt_add_subnode(machine->fdt, "/hetero-mcu");
    qemu_fdt_setprop_string(machine->fdt, "/hetero-mcu", "compatible",
                           "qemu,hetero-mcu");
    qemu_fdt_setprop_string(machine->fdt, "/hetero-mcu", "cpu-profile",
                            s->profile);
    qemu_fdt_setprop_cell(machine->fdt, "/hetero-mcu", "memory-region",
                          phandle);
}

static void mcu_finalize(Object *obj)
{
    HeteroVirtMCU *s = HETERO_VIRT_MCU(obj);

    if (s->cpu) {
        qemu_unregister_reset(mcu_reset, s);
    }
}

static const Property mcu_properties[] = {
    DEFINE_PROP_STRING("cpu-profile", HeteroVirtMCU, profile),
    DEFINE_PROP_STRING("firmware", HeteroVirtMCU, firmware),
    DEFINE_PROP_STRING("kernel", HeteroVirtMCU, kernel),
    DEFINE_PROP_STRING("append", HeteroVirtMCU, append),
    DEFINE_PROP_SIZE("ram-size", HeteroVirtMCU, ram_size,
                     HETERO_MCU_RAM_SIZE),
};

static void mcu_instance_init(Object *obj)
{
    HETERO_VIRT_MCU(obj)->profile = g_strdup("cortex-m3-arm-cpu");
}

static void mcu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mcu_realize;
    dc->user_creatable = false;
    dc->hotpluggable = false;
    device_class_set_props(dc, mcu_properties);
}

static const TypeInfo mcu_type = {
    .name = TYPE_HETERO_VIRT_MCU,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(HeteroVirtMCU),
    .instance_init = mcu_instance_init,
    .instance_finalize = mcu_finalize,
    .class_init = mcu_class_init,
};

static void mcu_register_types(void)
{
    type_register_static(&mcu_type);
}
type_init(mcu_register_types)
