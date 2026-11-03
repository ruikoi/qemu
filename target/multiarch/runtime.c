/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local experimental dispatch; ordinary builds do not compile this file. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/cpu.h"
#include "hw/core/boards.h"
#include "exec/cpu-common.h"
#include "qemu/target-info-impl.h"
#include "qemu/target-info-qom.h"
#include "qemu/multiarch-runtime.h"
#include "qapi/qapi-commands-machine.h"
#include "semihosting/common-semi.h"
#include "system/tcg.h"
#include "accel/tcg/cpu-ops.h"

bool multiarch_prepare_cpu_types(const char *const *types, size_t count,
                                 Error **errp)
{
    if (!tcg_enabled()) {
        error_setg(errp, "multiarch requires the TCG accelerator");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        ObjectClass *oc = object_class_by_name(types[i]);

        if (!oc || !object_class_dynamic_cast(oc, TYPE_CPU)) {
            error_setg(errp, "CPU type %s is unavailable in this build",
                       types[i]);
            return false;
        }
    }
    for (size_t i = 0; i < count; i++) {
        CPUClass *cc = CPU_CLASS(object_class_by_name(types[i]));

        if (qemu_tcg_mttcg_enabled() && !cc->tcg_ops->mttcg_supported) {
            error_setg(errp, "CPU type %s does not support MTTCG", types[i]);
            return false;
        }
    }
    /* Complete every frontend before vCPU threads clone tcg_init_ctx. */
    for (size_t i = 0; i < count; i++) {
        CPUState *scratch = CPU(object_new(types[i]));

        tcg_exec_initialize_target(scratch);
        object_unref(OBJECT(scratch));
    }
    return true;
}

const TargetInfo *target_info_for_cpu(CPUState *cs)
{
    g_autoptr(GSList) targets = object_class_get_list(TYPE_TARGET_INFO, false);
    for (GSList *it = targets; it; it = it->next) {
        const TargetInfo *ti = TARGET_INFO_CLASS(it->data)->target_info;
        if (object_dynamic_cast(OBJECT(cs), ti->cpu_type)) {
            return ti;
        }
    }
    g_assert_not_reached();
}

static const MultiarchRuntimeOps *ops_for_cpu(CPUState *cs)
{
    return target_info_for_cpu(cs)->runtime_ops;
}

const char *hetero_machine_cpu_type(MachineState *machine)
{
    const char *name = machine ? machine_default_cpu_type(machine) : NULL;
    ObjectClass *cpu_class = name ? object_class_by_name(name) : NULL;
    g_autoptr(GSList) targets = object_class_get_list(TYPE_TARGET_INFO, false);

    for (GSList *it = targets; cpu_class && it; it = it->next) {
        const TargetInfo *ti = TARGET_INFO_CLASS(it->data)->target_info;

        if (object_class_dynamic_cast(cpu_class, ti->cpu_type)) {
            return ti->cpu_type;
        }
    }
    return target_cpu_type();
}

SysEmuTarget hetero_cpu_arch(CPUState *cpu)
{
    return ops_for_cpu(cpu)->cpu_arch(cpu);
}
void hetero_cpu_set_arch_id(CPUState *cpu, unsigned index)
{
    ops_for_cpu(cpu)->set_arch_id(cpu, index);
}

uint64_t common_semi_arg(CPUState *cs, int argno)
{
    return ops_for_cpu(cs)->semi_arg(cs, argno);
}
void common_semi_set_ret(CPUState *cs, uint64_t ret)
{
    ops_for_cpu(cs)->semi_ret(cs, ret);
}

bool is_64bit_semihosting(CPUArchState *env)
{
    CPUState *cs = env_cpu(env);
    return ops_for_cpu(cs)->semi64(cs);
}
bool common_semi_sys_exit_is_extended(CPUState *cs)
{
    return ops_for_cpu(cs)->semi_exit(cs);
}
uint64_t common_semi_stack_bottom(CPUState *cs)
{
    return ops_for_cpu(cs)->semi_stack(cs);
}
bool common_semi_has_synccache(CPUArchState *env)
{
    CPUState *cs = env_cpu(env);
    return ops_for_cpu(cs)->semi_cache(cs);
}

CpuDefinitionInfoList *qmp_query_cpu_definitions(Error **errp)
{
    g_autoptr(GSList) targets = object_class_get_list(TYPE_TARGET_INFO, false);
    CpuDefinitionInfoList *head = NULL, **tail = &head;
    for (GSList *it = targets; it; it = it->next) {
        const TargetInfo *ti = TARGET_INFO_CLASS(it->data)->target_info;
        Error *local_err = NULL;
        *tail = ti->runtime_ops->definitions(&local_err);
        if (local_err) {
            qapi_free_CpuDefinitionInfoList(head);
            error_propagate(errp, local_err);
            return NULL;
        }
        while (*tail) {
            tail = &(*tail)->next;
        }
    }
    return head;
}

CpuModelExpansionInfo *qmp_query_cpu_model_expansion(CpuModelExpansionType type,
                                                    CpuModelInfo *model,
                                                    Error **errp)
{
    g_autoptr(GSList) targets = object_class_get_list(TYPE_TARGET_INFO, false);
    if (g_slist_length(targets) != 1) {
        error_setg(errp, "CPU model expansion is unavailable in the hetero prototype");
        return NULL;
    }
    return target_info()->runtime_ops->expansion(type, model, errp);
}

static const MultiarchRuntimeOps *homogeneous_ops(void)
{
    const MultiarchRuntimeOps *ops = NULL;
    SysEmuTarget arch = 0;
    CPUState *cs;
    CPU_FOREACH(cs) {
        const MultiarchRuntimeOps *next = ops_for_cpu(cs);
        if (ops && (ops != next || arch != next->cpu_arch(cs))) {
            return NULL;
        }
        ops = next;
        arch = next->cpu_arch(cs);
    }
    return ops;
}

int cpu_get_dump_info(ArchDumpInfo *info, const GuestPhysBlockList *blocks)
{
    const MultiarchRuntimeOps *ops = homogeneous_ops();
    /* A single ELF machine type cannot describe this mixed-ISA CPU set. */
    return ops ? ops->dump_info(info, blocks) : -1;
}
ssize_t cpu_get_note_size(int class, int machine, int nr_cpus)
{
    const MultiarchRuntimeOps *ops = homogeneous_ops();
    return ops ? ops->note_size(class, machine, nr_cpus) : -1;
}
