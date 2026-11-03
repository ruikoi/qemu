/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local experiment: per-target adapters for formerly binary-global APIs. */
#ifndef QEMU_MULTIARCH_RUNTIME_H
#define QEMU_MULTIARCH_RUNTIME_H
#include "qapi/qapi-types-machine.h"
#include "system/dump.h"

/* Call before realizing any CPU or starting a vCPU thread. */
bool multiarch_prepare_cpu_types(const char *const *types, size_t count,
                                 Error **errp);

typedef struct MultiarchRuntimeOps {
    SysEmuTarget (*cpu_arch)(CPUState *);
    void (*set_arch_id)(CPUState *, unsigned);
    uint64_t (*semi_arg)(CPUState *, int);
    void (*semi_ret)(CPUState *, uint64_t);
    bool (*semi64)(CPUState *);
    bool (*semi_exit)(CPUState *);
    uint64_t (*semi_stack)(CPUState *);
    bool (*semi_cache)(CPUState *);
    CpuDefinitionInfoList *(*definitions)(Error **);
    CpuModelExpansionInfo *(*expansion)(CpuModelExpansionType, CpuModelInfo *, Error **);
    int (*dump_info)(ArchDumpInfo *, const GuestPhysBlockList *);
    ssize_t (*note_size)(int, int, int);
} MultiarchRuntimeOps;
/* Architecture-prefixed dump implementations used by each adapter table. */
int arm_hetero_cpu_get_dump_info(ArchDumpInfo *, const GuestPhysBlockList *);
int riscv_hetero_cpu_get_dump_info(ArchDumpInfo *, const GuestPhysBlockList *);
ssize_t arm_hetero_cpu_get_note_size(int, int, int);
ssize_t riscv_hetero_cpu_get_note_size(int, int, int);
SysEmuTarget hetero_cpu_arch(CPUState *cpu);
const char *hetero_machine_cpu_type(MachineState *machine);
void hetero_cpu_set_arch_id(CPUState *cpu, unsigned index);
#endif
