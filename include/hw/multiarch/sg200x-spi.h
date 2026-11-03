/*
 * Milk-V DUO Board SPI Controller
 *
 * Copyright (c) 2025 Milk-V, Inc.
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

#ifndef HW_SG200X_SPI_H
#define HW_SG200X_SPI_H

#include "hw/ssi/ssi.h"
#include "hw/core/sysbus.h"

#define SG200X_SPI_REG_NUM 64

#define TYPE_SG200X_SPI "sg200x.spi"
#define SG200X_SPI(obj) OBJECT_CHECK(SG200XSPIState, (obj), TYPE_SG200X_SPI)

typedef struct SG200XSPIState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t num_cs;
    char *bus_name;
    qemu_irq *cs_lines;

    SSIBus *spi;

    uint16_t tx[8], rx[8];
    unsigned tx_count, rx_count;
    uint32_t errors, rx_remaining;
    bool read_started;

    uint32_t regs[SG200X_SPI_REG_NUM];

} SG200XSPIState;

#endif /* HW_SG200X_SPI_H */
