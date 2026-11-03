/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Local multiarch hardware composition interfaces. */
#ifndef HW_MULTIARCH_DOMAIN_H
#define HW_MULTIARCH_DOMAIN_H

#include "hw/core/cpu.h"
#include "system/memory.h"
#include "chardev/char.h"

/* Board-owned wiring: no controller is shared implicitly across domains. */
typedef struct MultiarchDomainConfig {
    unsigned id;
    unsigned num_irqs;
    hwaddr resetvec;
    hwaddr uart;
    hwaddr swi;
    hwaddr mtimer;
    hwaddr plic;
    uint64_t clock_hz;
    uint64_t time_hz;
    Chardev *serial;
} MultiarchDomainConfig;

typedef struct MultiarchCoreProfile {
    const char *name;
    const char *cpu_type;
    bool vector_alias;
    bool linux_m_mode;
    CPUState *(*create_cpu)(Object *parent, MemoryRegion *memory,
                            const MultiarchDomainConfig *config);
    void (*create_devices)(CPUState *cpu, MemoryRegion *memory,
                            const MultiarchDomainConfig *config);
} MultiarchCoreProfile;

/* Static-lifetime profiles register during QOM module initialization. */
void multiarch_core_profile_register(const MultiarchCoreProfile *profile);
const MultiarchCoreProfile *multiarch_core_profile_find(const char *name);
CPUState *multiarch_domain_create(Object *parent, MemoryRegion *memory,
                                  const MultiarchCoreProfile *profile,
                                  const MultiarchDomainConfig *config);
/* Optional default controller wiring; a SoC may connect its own devices. */
void multiarch_domain_create_devices(CPUState *cpu, MemoryRegion *memory,
                                     const MultiarchCoreProfile *profile,
                                     const MultiarchDomainConfig *config);
#endif
