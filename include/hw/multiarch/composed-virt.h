/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MULTIARCH_COMPOSED_VIRT_H
#define HW_MULTIARCH_COMPOSED_VIRT_H
#include "hw/core/boards.h"
#include "system/memory.h"
#include "chardev/char.h"

/* A domain owns an address space and a contiguous group of vCPUs. */
typedef struct MultiarchVirtDomain {
    char *id;
    char *platform;
    char *cpu_type;
    unsigned index, cpu_base, num_cpus;
    uint64_t ram_size, ram_base, entry;
    char *kernel, *initrd, *firmware, *append;
    Chardev *serial;
    Object *cluster;
    MemoryRegion root, ram, vectors;
    CPUState **cpus;
    void *fdt;
    void *opaque;
} MultiarchVirtDomain;

unsigned multiarch_virt_cpu_count(void);
void multiarch_virt_validate_machine(MachineState *machine);
void multiarch_virt_arm_create(MultiarchVirtDomain *d);
void multiarch_virt_arm_boot(MultiarchVirtDomain *d, MachineState *machine);
void multiarch_virt_riscv_create(MultiarchVirtDomain *d);
void multiarch_virt_riscv_boot(MultiarchVirtDomain *d, MachineState *machine);
void multiarch_virt_fdt_base(MultiarchVirtDomain *d);
void multiarch_virt_map_device(MultiarchVirtDomain *d, DeviceState *dev,
                               unsigned region, hwaddr addr);
void multiarch_virt_load_firmware(MultiarchVirtDomain *d, unsigned elf_machine);
#endif
