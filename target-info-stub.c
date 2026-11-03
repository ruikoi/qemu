/*
 * QEMU target info stubs (target specific)
 *
 *  Copyright (c) Linaro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/target-info.h"
#include "qemu/target-info-impl.h"
#include "qemu/target-info-init.h"
#include "hw/core/boards.h"
#include "cpu.h"
#include "exec/cpu-defs.h"
#include "exec/page-vary.h"

/* Validate correct placement of CPUArchState. */
QEMU_BUILD_BUG_ON(offsetof(ArchCPU, parent_obj) != 0);
QEMU_BUILD_BUG_ON(offsetof(ArchCPU, env) != sizeof(CPUState));

/* Validate target page size, if invariant. */
#ifndef TARGET_PAGE_BITS_VARY
QEMU_BUILD_BUG_ON(TARGET_PAGE_BITS < TARGET_PAGE_BITS_MIN);
#endif

#ifdef CONFIG_HETERO_PROTOTYPE
#include "qemu/multiarch-runtime.h"
#if defined(TARGET_ARM)
#define HETERO_PREFIX arm_hetero_
#else
#define HETERO_PREFIX riscv_hetero_
#endif
#include "qemu/hetero-namespace.h"
#include "semihosting/common-semi.h"
#include "qapi/qapi-commands-machine.h"

static bool hetero_semi64(CPUState *cs)
{
    return is_64bit_semihosting(cpu_env(cs));
}
static bool hetero_semi_cache(CPUState *cs)
{
    return common_semi_has_synccache(cpu_env(cs));
}
static SysEmuTarget hetero_cpu_actual_arch(CPUState *cs)
{
#if defined(TARGET_ARM)
    return arm_feature(cpu_env(cs), ARM_FEATURE_AARCH64) ?
           SYS_EMU_TARGET_AARCH64 : SYS_EMU_TARGET_ARM;
#else
    return riscv_cpu_max_xlen(RISCV_CPU_GET_CLASS(cs)) == 32 ?
           SYS_EMU_TARGET_RISCV32 : SYS_EMU_TARGET_RISCV64;
#endif
}
static void hetero_cpu_arch_id(CPUState *cs, unsigned index)
{
#if defined(TARGET_RISCV)
    RISCV_CPU(cs)->env.mhartid = index;
#endif
}
static const MultiarchRuntimeOps hetero_ops = {
    .cpu_arch = hetero_cpu_actual_arch,
    .set_arch_id = hetero_cpu_arch_id,
    .semi_arg = common_semi_arg,
    .semi_ret = common_semi_set_ret,
    .semi64 = hetero_semi64,
    .semi_exit = common_semi_sys_exit_is_extended,
    .semi_stack = common_semi_stack_bottom,
    .semi_cache = hetero_semi_cache,
    .definitions = qmp_query_cpu_definitions,
    .expansion = qmp_query_cpu_model_expansion,
    .dump_info = cpu_get_dump_info,
    .note_size = cpu_get_note_size,
};
#endif

static const TargetInfo target_info_stub = {
#ifdef CONFIG_HETERO_PROTOTYPE
    .runtime_ops = &hetero_ops,
#endif
    .target_name = TARGET_NAME,
    .target_arch = glue(SYS_EMU_TARGET_, TARGET_ARCH),
    .long_bits = TARGET_LONG_BITS,
    .cpu_type = CPU_RESOLVING_TYPE,
    .machine_typename = TYPE_MACHINE,
    .endianness = TARGET_BIG_ENDIAN ? ENDIAN_MODE_BIG : ENDIAN_MODE_LITTLE,
#ifdef TARGET_PAGE_BITS_VARY
    .page_bits_vary = true,
# ifdef TARGET_PAGE_BITS_LEGACY
    .page_bits_init = TARGET_PAGE_BITS_LEGACY,
# endif
#else
    .page_bits_vary = false,
    .page_bits_init = TARGET_PAGE_BITS,
#endif
};

target_info_init(target_info_stub)
