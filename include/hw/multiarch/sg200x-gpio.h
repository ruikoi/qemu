/*
 * Milk-V DUO Board General Purpose Input/Output Controller
 *
 * Copyright (c) 2025 Milkv, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_SG200X_GPIO_H
#define HW_SG200X_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SG200X_GPIO "sg200x.gpio"
#define SG200X_GPIO(obj) \
    OBJECT_CHECK(SG200XGPIOState, (obj), TYPE_SG200X_GPIO)

#define SG200X_GPIO_PINS 32

#define SG200X_GPIO_SIZE 0x100

#define SG200X_GPIO_REG_SWPORTA_DR      0x000
#define SG200X_GPIO_REG_SWPORTA_DDR     0x004
#define SG200X_GPIO_REG_INTEN           0x030
#define SG200X_GPIO_REG_INTMASK         0x034
#define SG200X_GPIO_REG_INTTYPE_LEVEL   0x038
#define SG200X_GPIO_REG_INT_POLARITY    0x03C
#define SG200X_GPIO_REG_INTSTATUS       0x040
#define SG200X_GPIO_REG_RAW_INTSTATUS   0x044
#define SG200X_GPIO_REG_DEBOUNCE        0x048
#define SG200X_GPIO_REG_PORTA_EOI       0x04C
#define SG200X_GPIO_REG_EXT_PORTA       0x050
#define SG200X_GPIO_REG_LS_SYNC         0x060

typedef struct SG200XGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    qemu_irq irq;
    qemu_irq output[SG200X_GPIO_PINS];

    uint32_t value; /* Actual value of the pin */

    uint32_t data;
    uint32_t dr;             /* Actual value of the pin */
    uint32_t ddr;
    uint32_t int_en;
    uint32_t int_mask;
    uint32_t int_level;
    uint32_t int_polar;
    uint32_t int_status;
    uint32_t int_raw_status;
    uint32_t debounce;
    uint32_t eoi;
    uint32_t ls_sync;
    uint32_t in;

    /* config */
    uint32_t ngpio;

} SG200XGPIOState;

#endif /* SG200X_GPIO_H */
