/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included after osdep, before interface declarations. */
#ifdef CONFIG_HETERO_PROTOTYPE
#define HETERO_SYM(name) glue(HETERO_PREFIX, name)
#define common_semi_arg HETERO_SYM(common_semi_arg)
#define common_semi_set_ret HETERO_SYM(common_semi_set_ret)
#define common_semi_set_err HETERO_SYM(common_semi_set_err)
#define is_64bit_semihosting HETERO_SYM(is_64bit_semihosting)
#define common_semi_sys_exit_is_extended HETERO_SYM(common_semi_sys_exit_is_extended)
#define common_semi_stack_bottom HETERO_SYM(common_semi_stack_bottom)
#define common_semi_has_synccache HETERO_SYM(common_semi_has_synccache)
#define qmp_query_cpu_definitions HETERO_SYM(qmp_query_cpu_definitions)
#define qmp_query_cpu_model_expansion HETERO_SYM(qmp_query_cpu_model_expansion)
#define cpu_get_dump_info HETERO_SYM(cpu_get_dump_info)
#define cpu_get_note_size HETERO_SYM(cpu_get_note_size)
#endif
