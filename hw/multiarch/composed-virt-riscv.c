/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ISA-local implementation of configurable RISC-V domains. */
#include "qemu/osdep.h"
#include "elf.h"
#include <libfdt.h>
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/multiarch-runtime.h"
#include "hw/core/sysbus.h"
#include "hw/core/loader.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/multiarch/composed-virt.h"
#include "system/device_tree.h"
#include "system/reset.h"

typedef struct RiscvDomainBoot {
    uint64_t dtb, info;
} RiscvDomainBoot;
static void riscv_domain_reset(void *opaque)
{
    MultiarchVirtDomain *d = opaque;
    RiscvDomainBoot *boot = d->opaque;
    for (unsigned i = 0; i < d->num_cpus; i++) {
        CPUState *cs = d->cpus[i];
        cpu_reset(cs);
        cpu_set_pc(cs, d->entry);
        RISCV_CPU(cs)->env.gpr[10] = d->cpu_base + i;
        if (boot) {
            RISCV_CPU(cs)->env.gpr[11] = boot->dtb;
            RISCV_CPU(cs)->env.gpr[12] = boot->info;
        }
        if (!d->firmware && !d->kernel) {
            cs->halted = 1;
        }
    }
}
void multiarch_virt_riscv_create(MultiarchVirtDomain *d)
{
    bool mcu = !strcmp(d->platform, "riscv-mcu");
    if (strcmp(d->cpu_type, mcu ? "rv32-riscv-cpu" : "rv64-riscv-cpu")) {
        error_report(
            "domain %s: riscv-mcu requires rv32, riscv-virt requires rv64",
            d->id);
        exit(EXIT_FAILURE);
    }
    for (unsigned i = 0; i < d->num_cpus; i++) {
        CPUState *cs = CPU(object_new(d->cpu_type));
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        object_property_add_child(d->cluster, name, OBJECT(cs));
        object_property_set_link(OBJECT(cs), "memory", OBJECT(&d->root),
                                 &error_fatal);
        object_property_set_int(OBJECT(cs), "resetvec", d->ram_base,
                                &error_fatal);
        object_property_set_bool(OBJECT(cs), "h", false, &error_fatal);
        if (mcu) {
            object_property_set_bool(OBJECT(cs), "mmu", false, &error_fatal);
            object_property_set_bool(OBJECT(cs), "s", false, &error_fatal);
        }
        hetero_cpu_set_arch_id(cs, d->cpu_base + i);
        qdev_realize(DEVICE(cs), NULL, &error_fatal);
        d->cpus[i] = cs;
        object_unref(OBJECT(cs));
    }
    riscv_aclint_swi_create_with_memory(&d->root, 0x02000000, d->cpu_base, d->num_cpus,
                            false);
    riscv_aclint_mtimer_create_with_memory(&d->root, 0x02004000, 0x8000, d->cpu_base,
                               d->num_cpus, 0, 0x7ff8, 10000000, true);
    GString *contexts = g_string_new(NULL);
    for (unsigned i = 0; i < d->num_cpus; i++) {
        g_string_append(contexts, i ? "," : "");
        g_string_append(contexts, mcu ? "M" : "MS");
    }
    DeviceState *plic = sifive_plic_create_with_memory(
        &d->root, 0x0c000000, contexts->str, d->num_cpus, d->cpu_base, 31, 7, 0,
        0x1000, 0x2000, 0x80, 0x200000, 0x1000, 0x400000);
    g_string_free(contexts, true);
    uint64_t uart = mcu ? 0x40000000 : 0x10000000;
    serial_mm_init(&d->root, uart, 0, qdev_get_gpio_in(plic, 1), 115200,
                   d->serial, DEVICE_LITTLE_ENDIAN);
    if (mcu) {
        return;
    }
    multiarch_virt_fdt_base(d);
    qemu_fdt_setprop_string(d->fdt, "/chosen", "bootargs",
                            d->append ? d->append
                                      : "console=ttyS0 rdinit=/init");
    qemu_fdt_setprop_cell(d->fdt, "/cpus", "timebase-frequency", 10000000);
    GArray *clint = g_array_new(false, false, sizeof(uint32_t));
    GArray *plic_ext = g_array_new(false, false, sizeof(uint32_t));
    for (unsigned i = 0; i < d->num_cpus; i++) {
        g_autofree char *node =
            g_strdup_printf("/cpus/cpu@%x", d->cpu_base + i);
        g_autofree char *intc =
            g_strdup_printf("%s/interrupt-controller", node);
        qemu_fdt_add_subnode(d->fdt, node);
        qemu_fdt_setprop_string(d->fdt, node, "device_type", "cpu");
        qemu_fdt_setprop_string(d->fdt, node, "compatible", "riscv");
        qemu_fdt_setprop_string(d->fdt, node, "status", "okay");
        qemu_fdt_setprop_cell(d->fdt, node, "reg", d->cpu_base + i);
        qemu_fdt_setprop_string(d->fdt, node, "riscv,isa",
                                "rv64imafdc_zicsr_zifencei");
        qemu_fdt_setprop_string(d->fdt, node, "mmu-type", "riscv,sv39");
        qemu_fdt_add_subnode(d->fdt, intc);
        qemu_fdt_setprop_string(d->fdt, intc, "compatible", "riscv,cpu-intc");
        qemu_fdt_setprop_cell(d->fdt, intc, "#interrupt-cells", 1);
        qemu_fdt_setprop(d->fdt, intc, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(d->fdt, intc, "phandle", i + 10);
        uint32_t c[] = {cpu_to_be32(i + 10), cpu_to_be32(3),
                        cpu_to_be32(i + 10), cpu_to_be32(7)};
        uint32_t p[] = {cpu_to_be32(i + 10), cpu_to_be32(11),
                        cpu_to_be32(i + 10), cpu_to_be32(9)};
        g_array_append_vals(clint, c, 4);
        g_array_append_vals(plic_ext, p, 4);
    }
    qemu_fdt_add_subnode(d->fdt, "/soc");
    qemu_fdt_setprop_string(d->fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(d->fdt, "/soc", "#address-cells", 2);
    qemu_fdt_setprop_cell(d->fdt, "/soc", "#size-cells", 2);
    qemu_fdt_setprop(d->fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_add_subnode(d->fdt, "/soc/clint@2000000");
    qemu_fdt_setprop_string(d->fdt, "/soc/clint@2000000", "compatible",
                            "riscv,clint0");
    qemu_fdt_setprop_sized_cells(d->fdt, "/soc/clint@2000000", "reg", 2,
                                 0x02000000, 2, 0x10000);
    qemu_fdt_setprop(d->fdt, "/soc/clint@2000000", "interrupts-extended",
                     clint->data, clint->len * 4);
    qemu_fdt_add_subnode(d->fdt, "/soc/plic@c000000");
    qemu_fdt_setprop_string(d->fdt, "/soc/plic@c000000", "compatible",
                            "riscv,plic0");
    qemu_fdt_setprop_sized_cells(d->fdt, "/soc/plic@c000000", "reg", 2,
                                 0x0c000000, 2, 0x400000);
    qemu_fdt_setprop_cell(d->fdt, "/soc/plic@c000000", "#interrupt-cells", 1);
    qemu_fdt_setprop(d->fdt, "/soc/plic@c000000", "interrupt-controller", NULL,
                     0);
    qemu_fdt_setprop_cell(d->fdt, "/soc/plic@c000000", "riscv,ndev", 31);
    qemu_fdt_setprop_cell(d->fdt, "/soc/plic@c000000", "phandle", 1);
    qemu_fdt_setprop(d->fdt, "/soc/plic@c000000", "interrupts-extended",
                     plic_ext->data, plic_ext->len * 4);
    g_array_free(clint, true);
    g_array_free(plic_ext, true);
    qemu_fdt_add_subnode(d->fdt, "/soc/serial@10000000");
    qemu_fdt_setprop_string(d->fdt, "/soc/serial@10000000", "compatible",
                            "ns16550a");
    qemu_fdt_setprop_sized_cells(d->fdt, "/soc/serial@10000000", "reg", 2,
                                 0x10000000, 2, 8);
    qemu_fdt_setprop_cell(d->fdt, "/soc/serial@10000000", "clock-frequency",
                          1843200);
    qemu_fdt_setprop_cell(d->fdt, "/soc/serial@10000000", "interrupt-parent",
                          1);
    qemu_fdt_setprop_cell(d->fdt, "/soc/serial@10000000", "interrupts", 1);
    qemu_fdt_setprop_string(d->fdt, "/chosen", "stdout-path",
                            "/soc/serial@10000000");
}
void multiarch_virt_riscv_boot(MultiarchVirtDomain *d, MachineState *machine)
{
    if (d->kernel && !d->firmware) {
        error_report("domain %s: riscv-virt kernel requires OpenSBI fw_dynamic "
                     "via firmware=",
                     d->id);
        exit(EXIT_FAILURE);
    }
    if (d->firmware) {
        uint64_t ram_size = d->ram_size;
        if (d->kernel) {
            d->ram_size = 2 * MiB;
        }
        multiarch_virt_load_firmware(d, EM_RISCV);
        d->ram_size = ram_size;
    } else {
        d->entry = d->ram_base;
    }
    if (d->kernel) {
        RiscvDomainBoot *boot = g_new0(RiscvDomainBoot, 1);
        if (d->ram_size < 32 * MiB) {
            error_report("RISC-V Linux domain requires at least 32 MiB RAM");
            exit(EXIT_FAILURE);
        }
        boot->dtb = d->ram_base + d->ram_size - MiB;
        boot->info = boot->dtb + 0x10000;
        int64_t length = load_image_targphys_as(
            d->kernel, d->ram_base + 2 * MiB, d->ram_size - 4 * MiB,
            d->cpus[0]->as, &error_fatal);
        if (length <= 0) {
            error_report("cannot load RISC-V Linux Image");
            exit(EXIT_FAILURE);
        }
        if (d->initrd) {
            uint64_t initrd =
                QEMU_ALIGN_UP(d->ram_base + 2 * MiB + length, 2 * MiB);
            if (initrd >= boot->dtb) {
                error_report("RISC-V initrd does not fit private RAM");
                exit(EXIT_FAILURE);
            }
            length = load_image_targphys_as(d->initrd, initrd,
                                           boot->dtb - initrd, d->cpus[0]->as,
                                           &error_fatal);
            if (length <= 0) {
                error_report("RISC-V initrd does not fit private RAM");
                exit(EXIT_FAILURE);
            }
            qemu_fdt_setprop_sized_cells(d->fdt, "/chosen",
                                         "linux,initrd-start", 2, initrd);
            qemu_fdt_setprop_sized_cells(d->fdt, "/chosen", "linux,initrd-end",
                                         2, initrd + length);
        }
        fdt_add_mem_rsv(d->fdt, boot->dtb, MiB);
        if (fdt_pack(d->fdt) || fdt_totalsize(d->fdt) > 0x10000) {
            error_report("RISC-V domain DTB too large");
            exit(EXIT_FAILURE);
        }
        uint64_t info[] = {0x4942534f, 2, d->ram_base + 2 * MiB,
                           1,          0, d->cpu_base};
        for (unsigned i = 0; i < ARRAY_SIZE(info); i++) {
            info[i] = cpu_to_le64(info[i]);
        }
        rom_add_blob_fixed_as(d->id, d->fdt, fdt_totalsize(d->fdt), boot->dtb,
                              d->cpus[0]->as);
        rom_add_blob_fixed_as(d->id, info, sizeof(info), boot->info,
                              d->cpus[0]->as);
        d->opaque = boot;
    }
    qemu_register_reset(riscv_domain_reset, d);
}
