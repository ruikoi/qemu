# SPDX-License-Identifier: GPL-2.0-or-later
# ISA-dependent adapters retain each ISA's compilation context.
if get_option('hetero_prototype')
  system_ss.add(files('domain.c'))
  arm_common_ss.add(files('domain-arm.c'))
  riscv_ss.add(files('domain-riscv.c'))
endif
