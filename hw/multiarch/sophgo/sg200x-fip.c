/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit fast boot: DDR/PLL are initialized by the emulator, not FSBL. */
#include "qemu/osdep.h"
#include <lzma.h>
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "hw/core/loader.h"
#include "hw/multiarch/sg200x.h"

#define PARAM_SIZE 4096
#define MAX_LOADER (16 * 1024 * 1024)

static uint32_t image_crc(const uint8_t *p, size_t size)
{
    uint16_t crc = 0;

    while (size--) {
        crc ^= (uint16_t)*p++ << 8;
        for (unsigned i = 0; i < 8; i++) {
            crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return 0xcafe0000U | crc;
}

static G_NORETURN void bad_fip(const char *reason)
{
    error_report("SG200X FIP: %s", reason);
    exit(EXIT_FAILURE);
}

static const uint8_t *slice(const uint8_t *file, size_t length,
                            uint32_t offset, size_t size)
{
    if (offset > length || size > length - offset) {
        bad_fip("payload outside file");
    }
    return file + offset;
}

static bool in_ddr(SG200XSoCState *s, hwaddr addr, size_t size, bool main)
{
    uint64_t end = SG200X_DDR + s->ram_size - (main ? SG200X_RTOS_SIZE : 0);

    return addr >= SG200X_DDR && addr <= end && size <= end - addr;
}

void sg200x_fip_load(SG200XSoCState *s, const char *name,
                     hwaddr *monitor, hwaddr *next)
{
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) error = NULL;
    gsize length;
    const uint8_t *file, *p1, *p2, *payload;
    uint32_t size;
    uint8_t boot_metadata[64] = { 0 };
    hwaddr small, loader_addr;
    size_t loader_size;
    g_autofree uint8_t *loader = NULL;

    if (!g_file_get_contents(name, &contents, &length, &error)) {
        bad_fip(error->message);
    }
    file = (const uint8_t *)contents;
    p1 = slice(file, length, 0, PARAM_SIZE);
    if (memcmp(p1, "CVBL01\n", 8) ||
        image_crc(p1 + 16, 2048 - 16) != ldl_le_p(p1 + 12)) {
        bad_fip("invalid PARAM1 magic or CRC");
    }
    if (ldq_le_p(p1 + 0xb4) & 0x3c) {
        bad_fip("secure/encrypted images are not supported");
    }
    p2 = slice(file, length, ldl_le_p(p1 + 0xe0), PARAM_SIZE);
    if (memcmp(p2, "CVLD02\n", 8) ||
        image_crc(p2 + 12, PARAM_SIZE - 12) != ldl_le_p(p2 + 8)) {
        bad_fip("invalid PARAM2 magic or CRC");
    }
    /* Non-secure boot metadata normally populated by ROM/FSBL. */
    stl_le_p(boot_metadata + 4, 0x4d474e32); /* SD download source */
    rom_add_blob_fixed("sg200x.fip.boot-metadata", boot_metadata,
                       sizeof(boot_metadata), 0x0e000000);
    rom_add_blob_fixed("sg200x.fip.param1", p1, PARAM_SIZE, 0x0c039000);
    *monitor = (uint32_t)ldl_le_p(p2 + 60);
    size = ldl_le_p(p2 + 56);
    payload = slice(file, length, ldl_le_p(p2 + 52), size);
    if (!size || !in_ddr(s, *monitor, size, true) ||
        image_crc(payload, size) != ldl_le_p(p2 + 48)) {
        bad_fip("invalid monitor range or CRC");
    }
    rom_add_blob_fixed("sg200x.fip.monitor", payload, size, *monitor);

    payload = slice(file, length, ldl_le_p(p2 + 68), 32);
    size = ldl_le_p(payload + 12);
    payload = slice(file, length, ldl_le_p(p2 + 68), size);
    if (size < 32 || image_crc(payload + 12, size - 12) !=
        ldl_le_p(payload + 8)) {
        bad_fip("invalid loader size or CRC");
    }
    loader_addr = ldq_le_p(payload + 16);
    loader = g_malloc(MAX_LOADER);
    memcpy(loader, payload, 32);
    if (ldl_le_p(payload + 4) == 0x414d3342) { /* B3MA / LZMA-alone */
        lzma_stream stream = LZMA_STREAM_INIT;
        lzma_ret result = lzma_alone_decoder(&stream, 64 * 1024 * 1024);

        if (result != LZMA_OK) {
            bad_fip("cannot initialize LZMA decoder");
        }
        stream.next_in = payload + 32;
        stream.avail_in = size - 32;
        stream.next_out = loader + 32;
        stream.avail_out = MAX_LOADER - 32;
        result = lzma_code(&stream, LZMA_FINISH);
        loader_size = 32 + stream.total_out;
        lzma_end(&stream);
        if (result != LZMA_STREAM_END) {
            bad_fip("invalid or oversized LZMA loader");
        }
    } else if (ldl_le_p(payload + 4) == 0x20203342 ||
               !memcmp(payload + 4, "BL33", 4)) {
        if (size > MAX_LOADER) {
            bad_fip("oversized raw loader");
        }
        memcpy(loader, payload, size);
        loader_size = size;
    } else {
        bad_fip("unsupported loader compression");
    }
    if (!in_ddr(s, loader_addr, loader_size, true) ||
        (loader_addr < *monitor + ldl_le_p(p2 + 56) &&
         *monitor < loader_addr + loader_size)) {
        bad_fip("loader overlaps monitor or is outside main DDR");
    }
    rom_add_blob_fixed("sg200x.fip.loader", loader, loader_size, loader_addr);
    *next = loader_addr + 32;

    size = ldl_le_p(p2 + 40);
    small = (uint32_t)ldl_le_p(p2 + 44);
    if (size && !s->rtos) {
        uint64_t tail = SG200X_DDR + s->ram_size - SG200X_RTOS_SIZE;
        bool reserved = small >= tail && in_ddr(s, small, size, false);
        bool sram = small >= 0x0c000000 &&
                    small <= 0x0c000000 + s->tpu_size &&
                    size <= 0x0c000000 + s->tpu_size - small &&
                    (small + size <= 0x0c039000 || small >= 0x0c03a000);

        payload = slice(file, length, ldl_le_p(p2 + 36), size);
        if ((!reserved && !sram) || image_crc(payload, size) !=
            ldl_le_p(p2 + 32)) {
            bad_fip("invalid small-core range or CRC");
        }
        rom_add_blob_fixed("sg200x.fip.small", payload, size, small);
        s->rtos_entry = small;
        s->rtos_loaded = true;
    }
}
