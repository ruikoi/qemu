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

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "hw/core/qdev-properties.h"
#include "hw/multiarch/sg200x-timer.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/cpu.h"

#define PTIMER_POLICY                       \
    (PTIMER_POLICY_WRAP_AFTER_ONE_PERIOD |  \
     PTIMER_POLICY_CONTINUOUS_TRIGGER    |  \
     PTIMER_POLICY_NO_IMMEDIATE_TRIGGER  |  \
     PTIMER_POLICY_NO_IMMEDIATE_RELOAD   |  \
     PTIMER_POLICY_NO_COUNTER_ROUND_DOWN)


static inline void timerblock_update_irq(SG200XTimerBlock *tb)
{
    qemu_set_irq(tb->irq, tb->status && !(tb->control & 4));
}

static void timerblock_tick(void *opaque)
{
    SG200XTimerBlock *tb = (SG200XTimerBlock *)opaque;

    tb->status = 1;
    timerblock_update_irq(tb);
}

static uint64_t timerblock_read(void *opaque, hwaddr addr,
                                unsigned size)
{
    SG200XTimerBlock *tb = (SG200XTimerBlock *)opaque;
    switch (addr) {
    case 0: /* LoadCount */
        return tb->load_count;
    case 4: /* CurrentValue */
        return ptimer_get_count(tb->timer);
    case 8: /* ControlReg */
        return tb->control;
    case 12: /* EOI */
        tb->status = 0;
        qemu_set_irq(tb->irq, 0);
        return tb->status;
    case 16: /* IntStatus */
        return tb->status && !(tb->control & 4);
    default:
        return 0;
    }
}

static void timerblock_write(void *opaque, hwaddr addr,
                             uint64_t value, unsigned size)
{
    SG200XTimerBlock *tb = (SG200XTimerBlock *)opaque;
    uint32_t control = tb->control;
    switch (addr) {
    case 0: /* LoadCount */
        ptimer_transaction_begin(tb->timer);
        tb->load_count = value;
        if (tb->control & 2) {
            ptimer_set_limit(tb->timer, MAX(value, 1), 0);
        }
        ptimer_set_count(tb->timer, MAX(value, 1));
        ptimer_transaction_commit(tb->timer);
        break;
    case 8: /* ControlReg */
        ptimer_transaction_begin(tb->timer);
        if ((control & 3) != (value & 3)) {
            ptimer_stop(tb->timer);
            ptimer_set_limit(tb->timer, value & 2 ? MAX(tb->load_count, 1) :
                                                    UINT32_MAX, 0);
            if (value & 1) {
                ptimer_set_count(tb->timer, tb->load_count ?
                                 tb->load_count : (value & 2 ? 1 : UINT32_MAX));
                ptimer_run(tb->timer, 0);
            }
        }
        tb->control = value & 7;
        timerblock_update_irq(tb);
        ptimer_transaction_commit(tb->timer);
        break;
    default:
        return;
    }
}

static uint64_t sg200x_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    SG200XTimerState *s = (SG200XTimerState *)opaque;
    if (addr >= 0xa0) {
        uint32_t value = 0;
        unsigned i;

        for (i = 0; i < SG200X_TIMER_MAX_NUM; i++) {
            SG200XTimerBlock *tb = &s->timerblock[i];

            switch (addr) {
            case 0xa0:
                value |= (tb->status && !(tb->control & 4)) << i;
                break;
            case 0xa4:
                tb->status = 0;
                timerblock_update_irq(tb);
                break;
            case 0xa8:
                value |= tb->status << i;
                break;
            default:
                return 0;
            }
        }
        return value;
    } else {
        return timerblock_read(&s->timerblock[addr / 0x14], addr % 0x14, size);
    }
}

static void sg200x_timer_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    SG200XTimerState *s = (SG200XTimerState *)opaque;
    if (addr < 0xa0) {
        timerblock_write(&s->timerblock[addr / 0x14], addr % 0x14, value,
                         size);
    }
}

static const MemoryRegionOps sg200x_timer_ops = {
    .read = sg200x_timer_read,
    .write = sg200x_timer_write,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void timerblock_reset(SG200XTimerBlock *tb)
{
    tb->load_count = 0;
    tb->control = 0;
    tb->status = 0;
    if (tb->timer) {
        ptimer_transaction_begin(tb->timer);
        ptimer_stop(tb->timer);
        ptimer_set_limit(tb->timer, 0, 1);
        ptimer_set_freq(tb->timer, 25000000);
        ptimer_transaction_commit(tb->timer);
    }
    timerblock_update_irq(tb);
}

static void sg200x_timer_reset(DeviceState *dev)
{
    SG200XTimerState *s = SG200X_TIMER(dev);
    int i;

    for (i = 0; i < ARRAY_SIZE(s->timerblock); i++) {
        timerblock_reset(&s->timerblock[i]);
    }
}

static void sg200x_timer_init(Object *obj)
{
    SG200XTimerState *s = SG200X_TIMER(obj);

    memory_region_init_io(&s->iomem, obj, &sg200x_timer_ops, s, "sg200x_timer",
                          0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void sg200x_timer_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SG200XTimerState *s = SG200X_TIMER(dev);
    int i;

    for (i = 0; i < SG200X_TIMER_MAX_NUM; i++) {
        SG200XTimerBlock *tb = &s->timerblock[i];
        tb->timer = ptimer_init(timerblock_tick, tb, PTIMER_POLICY);
        sysbus_init_irq(sbd, &tb->irq);

    }
}

static const VMStateDescription vmstate_timerblock = {
    .name = "sg200x_timer_timerblock",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(load_count, SG200XTimerBlock),
        VMSTATE_UINT32(control, SG200XTimerBlock),
        VMSTATE_UINT32(status, SG200XTimerBlock),
        VMSTATE_PTIMER(timer, SG200XTimerBlock),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_sg200x_timer = {
    .name = "sg200x_timer",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(timerblock, SG200XTimerState,
                             SG200X_TIMER_MAX_NUM, 3, vmstate_timerblock,
                             SG200XTimerBlock),
        VMSTATE_END_OF_LIST()
    }
};


static void sg200x_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sg200x_timer_realize;
    dc->vmsd = &vmstate_sg200x_timer;
    dc->desc = "Milk-V SG200X Timer";
    device_class_set_legacy_reset(dc, sg200x_timer_reset);
}

static const TypeInfo sg200x_timer_info = {
    .name          = TYPE_SG200X_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SG200XTimerState),
    .instance_init = sg200x_timer_init,
    .class_init    = sg200x_timer_class_init,
};

static void sg200x_timer_register_types(void)
{
    type_register_static(&sg200x_timer_info);
}

type_init(sg200x_timer_register_types)
