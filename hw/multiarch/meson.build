# SPDX-License-Identifier: GPL-2.0-or-later
# ISA-dependent adapters retain each ISA's compilation context.
if get_option('hetero_prototype')
  system_ss.add(dependency('liblzma'))
  system_ss.add(files(
    'domain.c',
    'composed-virt.c',
    'virt.c',
    'sophgo/sg200x-timer.c',
    'sophgo/sg200x-gpio.c',
    'sophgo/sg200x-spi.c',
    'sophgo/sg200x-ephy.c',
    'sophgo/sg200x-eeprom.c',
    'sophgo/sg200x-soc.c',
    'sophgo/sg200x-fip.c',
    'sophgo/sg200x-board.c',
  ))
  arm_common_ss.add(files(
    'domain-arm.c',
    'composed-virt-arm.c',
    'virt-arm.c',
    'sophgo/sg200x-arm.c',
  ))
  riscv_ss.add(files(
    'domain-riscv.c',
    'composed-virt-riscv.c',
    'virt-riscv.c',
    'sophgo/sg200x-riscv.c',
  ))
endif
