/*
 * Milk-V DUO Board Timer
 *
 * Copyright (c) 2023  PLCT Lab
 * Copyright (c) 2025  2025 Milk-V, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef HW_SG200X_TIMER_H
#define HW_SG200X_TIMER_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define SG200X_TIMER_MAX_NUM 8

#define TYPE_SG200X_TIMER "sg200x.timer"
#define SG200X_TIMER(obj) \
    OBJECT_CHECK(SG200XTimerState, (obj), TYPE_SG200X_TIMER)

/* State of a single timer or watchdog block */
typedef struct SG200XTimerBlock {
    uint32_t load_count;
    uint32_t control;
    uint32_t status;
    struct ptimer_state *timer;
    qemu_irq irq;
    MemoryRegion iomem;
} SG200XTimerBlock;

typedef struct SG200XTimerState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    SG200XTimerBlock timerblock[SG200X_TIMER_MAX_NUM];

    MemoryRegion iomem;

} SG200XTimerState;

#endif
