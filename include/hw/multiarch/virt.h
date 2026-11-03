/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local composition experiment, not intended for upstream contribution. */
#ifndef HW_HETERO_VIRT_H
#define HW_HETERO_VIRT_H

#include "hw/core/boards.h"
#include "hw/core/qdev.h"

#define TYPE_HETERO_VIRT_MCU "hetero-virt-mcu"
OBJECT_DECLARE_SIMPLE_TYPE(HeteroVirtMCU, HETERO_VIRT_MCU)

#define HETERO_MCU_RAM_BASE   0x10000000ULL
#define HETERO_MCU_RAM_SIZE   0x00100000ULL
#define HETERO_MCU_SHARED     0x30000000ULL
#define HETERO_MCU_UART       0x40000000ULL
#define HETERO_SHARED_SIZE    0x00010000ULL
#define HETERO_MCU_CLOCK_HZ   100000000
#define HETERO_RISCV_TIME_HZ  10000000

HeteroVirtMCU *hetero_virt_mcu_new(MachineState *machine);
void hetero_virt_prepare(MachineState *machine, HeteroVirtMCU *mcu,
                         const char *main_cpu_type);
void hetero_virt_attach(MachineState *machine, HeteroVirtMCU *mcu,
                        hwaddr shared_addr);

CpuInstanceProperties hetero_virt_cpu_index_to_props(MachineState *machine,
                                                     unsigned cpu_index);

#endif
