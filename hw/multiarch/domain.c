/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/cpu/cluster.h"
#include "hw/multiarch/domain.h"
#include "system/tcg.h"

static GHashTable *profiles;

void multiarch_core_profile_register(const MultiarchCoreProfile *profile)
{
    assert(profile->name && profile->cpu_type && profile->create_cpu &&
           profile->create_devices);
    if (!profiles) {
        profiles = g_hash_table_new(g_str_hash, g_str_equal);
    }
    assert(!g_hash_table_contains(profiles, profile->name));
    g_hash_table_insert(profiles, (void *)profile->name, (void *)profile);
}

const MultiarchCoreProfile *multiarch_core_profile_find(const char *name)
{
    return profiles && name ? g_hash_table_lookup(profiles, name) : NULL;
}

CPUState *multiarch_domain_create(Object *parent, MemoryRegion *memory,
                                  const MultiarchCoreProfile *profile,
                                  const MultiarchDomainConfig *config)
{
    Object *cluster = object_new(TYPE_CPU_CLUSTER);
    CPUState *cpu;

    object_property_add_child(parent, "cluster", cluster);
    object_property_set_int(cluster, "cluster-id", config->id, &error_fatal);
    cpu = profile->create_cpu(cluster, memory, config);
    qdev_realize(DEVICE(cluster), NULL, &error_fatal);
    tcg_update_cflags(cpu);
    object_unref(cluster);
    return cpu;
}

void multiarch_domain_create_devices(CPUState *cpu, MemoryRegion *memory,
                                     const MultiarchCoreProfile *profile,
                                     const MultiarchDomainConfig *config)
{
    profile->create_devices(cpu, memory, config);
}
