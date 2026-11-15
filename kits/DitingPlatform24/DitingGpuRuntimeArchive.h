/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_GPU_RUNTIME_ARCHIVE_H
#define DITING_GPU_RUNTIME_ARCHIVE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Fixed destinations; the archive never supplies a filesystem path or mode.
 * Header: magic, ABI, chip, count, reserved, total bytes, 16-byte install ID.
 * Each entry: raw bytes, zlib bytes, stream padded to eight bytes. */
enum { DitingGpuRuntimeFiles = 8, DitingGpuRuntimeHeader = 48 };
#define DITING_GPU_RUNTIME_PACKED_MAX (64u << 20)
#define DITING_GPU_RUNTIME_RAW_MAX (128u << 20)
struct DitingGpuRuntimeEntry {
    const uint8_t *packed;
    uint64_t bytes, packed_bytes;
};
static inline uint64_t diting_gpu_runtime_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static inline uint32_t diting_gpu_runtime_u32(const uint8_t *p)
{
    return (uint32_t)diting_gpu_runtime_u64(p) & UINT32_MAX;
}
static inline int diting_gpu_runtime_parse(const uint8_t *data, size_t bytes,
    struct DitingGpuRuntimeEntry entries[DitingGpuRuntimeFiles])
{
    if (!data || !entries || bytes < DitingGpuRuntimeHeader ||
        bytes > DITING_GPU_RUNTIME_PACKED_MAX || memcmp(data, "DTGPU001", 8) ||
        diting_gpu_runtime_u32(data + 8) != 1 ||
        diting_gpu_runtime_u32(data + 12) != 0x07030002 ||
        diting_gpu_runtime_u32(data + 16) != DitingGpuRuntimeFiles ||
        diting_gpu_runtime_u32(data + 20) || diting_gpu_runtime_u64(data + 24) != bytes)
        return 0;
    unsigned id_nonzero = 0;
    for (unsigned i = 32; i < DitingGpuRuntimeHeader; ++i) id_nonzero |= data[i];
    if (!id_nonzero) return 0;
    size_t at = DitingGpuRuntimeHeader;
    uint64_t total_raw = 0;
    for (unsigned i = 0; i < DitingGpuRuntimeFiles; ++i) {
        if (bytes - at < 16) return 0;
        uint64_t raw = diting_gpu_runtime_u64(data + at);
        uint64_t packed = diting_gpu_runtime_u64(data + at + 8);
        at += 16;
        if (!raw || raw > DITING_GPU_RUNTIME_PACKED_MAX || !packed ||
            packed > DITING_GPU_RUNTIME_PACKED_MAX ||
            total_raw > DITING_GPU_RUNTIME_RAW_MAX - raw) return 0;
        total_raw += raw;
        uint64_t padded = (packed + 7) & ~UINT64_C(7);
        if (padded > bytes - at) return 0;
        for (uint64_t n = packed; n < padded; ++n) if (data[at + n]) return 0;
        entries[i].bytes = raw;
        entries[i].packed_bytes = packed;
        entries[i].packed = data + at;
        at += (size_t)padded;
    }
    return at == bytes;
}
#endif
