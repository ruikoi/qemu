/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CORAL_NPU_ABI_H
#define CORAL_NPU_ABI_H
#include <stdint.h>
#define CORAL_ABI_VERSION 1
#define CORAL_ITCM_SIZE 0x2000
#define CORAL_DTCM_SIZE 0x8000
#define CORAL_EXT_SIZE 0x400000
#define CORAL_OK 1
#define CORAL_TIMEOUT 2
#define CORAL_TRAP 3
#define CORAL_ERROR 4
typedef struct CoralRequest {
    uint8_t *itcm;
    uint8_t *dtcm;
    uint8_t *ext;
    uint32_t entry;
    uint32_t limit;
    uint64_t steps;
    char error[256];
} CoralRequest;
typedef uint32_t (*CoralVersionFn)(void);
typedef int (*CoralRunFn)(CoralRequest *);
#endif
