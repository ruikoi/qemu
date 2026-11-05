/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CORAL_GEMMA_ABI_H
#define CORAL_GEMMA_ABI_H
#include <stdint.h>
#define CORAL_GEMMA_ABI 2
#define CORAL_GEMMA_TCM_SIZE 0x200000u
#define CORAL_GEMMA_DDR_SIZE 0x20000000u
#define CORAL_GEMMA_DDR_BASE 0x20000000u
typedef struct {
    uint8_t *tcm, *ddr;
    uint32_t entry, limit;
    uint64_t steps, ddr_reads, ddr_writes;
    char error[256];
} CoralGemmaRequest;
typedef uint32_t (*CoralGemmaVersionFn)(void);
typedef int (*CoralGemmaRunFn)(CoralGemmaRequest *);
#endif
