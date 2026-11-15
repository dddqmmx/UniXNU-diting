/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_UI_RUNTIME_ARCHIVE_H
#define DITING_UI_RUNTIME_ARCHIVE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* One fixed destination: /diting/libditing-ui18.dylib. No archive paths.
 * Header: magic, ABI, board, count, reserved, total bytes, 16-byte ID.
 * Entry: raw bytes, zlib bytes, stream padded to eight bytes. */
enum { DitingUiRuntimeHeader = 48, DitingUiRuntimeRawMax = 8u << 20,
       DitingUiRuntimePackedMax = 4u << 20 };
struct DitingUiRuntimeEntry {
    const uint8_t *packed;
    uint64_t bytes, packed_bytes;
};
static inline uint64_t diting_ui_runtime_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static inline uint32_t diting_ui_runtime_u32(const uint8_t *p)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (8 * i);
    return value;
}
static inline int diting_ui_runtime_parse(const uint8_t *data, size_t bytes,
    struct DitingUiRuntimeEntry *entry)
{
    if (!data || !entry || bytes < 64 || bytes > DitingUiRuntimePackedMax ||
        memcmp(data, "DTUI0001", 8) || diting_ui_runtime_u32(data + 8) != 1 ||
        diting_ui_runtime_u32(data + 12) != 0x8475 ||
        diting_ui_runtime_u32(data + 16) != 1 || diting_ui_runtime_u32(data + 20) ||
        diting_ui_runtime_u64(data + 24) != bytes) return 0;
    unsigned nonzero = 0;
    for (unsigned i = 32; i < DitingUiRuntimeHeader; ++i) nonzero |= data[i];
    if (!nonzero) return 0;
    uint64_t raw = diting_ui_runtime_u64(data + 48);
    uint64_t packed = diting_ui_runtime_u64(data + 56);
    if (raw < 32 || raw > DitingUiRuntimeRawMax || !packed ||
        packed > DitingUiRuntimePackedMax) return 0;
    uint64_t padded = (packed + 7) & ~UINT64_C(7);
    if (padded != bytes - 64) return 0;
    for (uint64_t n = packed; n < padded; ++n) if (data[64 + n]) return 0;
    entry->packed = data + 64;
    entry->bytes = raw;
    entry->packed_bytes = packed;
    return 1;
}
#endif
