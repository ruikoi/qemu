/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MULTIARCH_SG200X_H
#define HW_MULTIARCH_SG200X_H

#include "hw/core/boards.h"
#include "hw/core/qdev.h"
#include "qemu/timer.h"
#include "hw/multiarch/domain.h"

#define TYPE_SG200X_SOC "sg200x-soc"
OBJECT_DECLARE_SIMPLE_TYPE(SG200XSoCState, SG200X_SOC)
#define SG200X_DDR 0x80000000ULL
#define SG200X_ROM 0x04400000ULL
#define SG200X_UART0 0x04140000ULL
#define SG200X_UART1 0x04150000ULL
#define SG200X_PLIC 0x70000000ULL
#define SG200X_CLINT 0x74000000ULL
#define SG200X_TIME_HZ 25000000
#define SG200X_RTOS_SIZE (8 * 1024 * 1024)

struct SG200XSoCState {
    DeviceState parent_obj;
    bool arm, rom_boot;
    uint64_t rom_base, rom_size, tpu_size;
    uint64_t ram_size;
    char *rtos;
    CPUState *main_cpu, *small_cpu;
    DeviceState *main_intc, *small_intc;
    Object *main_domain, *small_domain;
    MemoryRegion small_root, shared_view, tpu_sram, rom;
    MemoryRegion rtc_sram, axi_sram, top_mmio, tpu_shadow;
    uint32_t top_gp[4];
    MemoryRegion ap_ctrl_mmio;
    uint32_t ap_pwr_iso[2];
    MemoryRegion rtc_core_mmio, rtc_ctrl_mmio;
    uint32_t rtc_core[1024], rtc_ctrl[64];
    uint32_t rtc_seconds;
    int64_t rtc_epoch_ns;
    uint32_t rtc_vbat;
    MemoryRegion audio_cfg_mmio, dma_cfg_mmio, usb_phy_mmio;
    uint32_t usb_phy[32];
    MemoryRegion thermal_mmio;
    uint32_t thermal[64];
    MemoryRegion watchdog_mmio;
    QEMUTimer *watchdog_timer;
    qemu_irq watchdog_irq;
    uint32_t watchdog_control, watchdog_range, watchdog_status;
    bool watchdog_expired;
    uint32_t dma_cfg[1024];
    uint32_t audio_cfg[64];
    MemoryRegion i2s_mmio[4], codec_mmio;
    uint32_t i2s_cfg[4][64], codec_cfg[128];
    MemoryRegion sd_vendor_mmio[3];
    uint32_t sd_vendor[3][20];
    MemoryRegion pinmux_mmio, rtc_pinmux_mmio, pad_mmio;
    uint32_t pad[1024];
    uint32_t pinmux[1024], rtc_pinmux[1024];
    MemoryRegion reset_mmio, secure_mmio, uart_aux[5];
    uint32_t resetn, secure_ctrl, entry_lo, entry_hi;
    uint64_t rtos_entry;
    bool rtos_loaded;
    Error *migration_blocker;
    /* ISA-specific boot information must outlive machine initialization. */
    void *boot_info;
};

void sg200x_soc_attach_small(SG200XSoCState *s);
void sg200x_riscv_intc(SG200XSoCState *s, bool small);
void sg200x_arm_intc(SG200XSoCState *s);
void sg200x_riscv_boot(SG200XSoCState *s, MachineState *machine);
void sg200x_fip_load(SG200XSoCState *s, const char *name,
                     hwaddr *monitor, hwaddr *next);
void sg200x_riscv_fip_boot(SG200XSoCState *s, MachineState *machine,
                           hwaddr monitor, hwaddr next);
void sg200x_arm_boot(SG200XSoCState *s, MachineState *machine);
#endif
