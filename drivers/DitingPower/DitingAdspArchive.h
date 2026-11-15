/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_ADSP_ARCHIVE_H
#define DITING_ADSP_ARCHIVE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Include the platform zlib header before this file. Compression only changes
 * kernel storage; PAS still receives the original signed ELF and metadata. */
enum { DitingAdspArchiveHeader = 32, DitingAdspArchiveLimit = 64u << 20 };
struct DitingAdspArchive {
    const uint8_t *packed;
    uint64_t bytes, packed_bytes;
};
static inline uint64_t diting_adsp_archive_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static inline int diting_adsp_archive_parse(const uint8_t *data, size_t bytes,
    struct DitingAdspArchive *entry)
{
    if (!data || !entry || bytes < DitingAdspArchiveHeader ||
        bytes > DitingAdspArchiveLimit + DitingAdspArchiveHeader ||
        memcmp(data, "DTADSP01", 8) || diting_adsp_archive_u64(data + 8) != 1) return 0;
    uint64_t raw = diting_adsp_archive_u64(data + 16);
    uint64_t packed = diting_adsp_archive_u64(data + 24);
    if (!raw || raw > DitingAdspArchiveLimit || !packed ||
        packed > DitingAdspArchiveLimit || packed != bytes - DitingAdspArchiveHeader) return 0;
    entry->packed = data + DitingAdspArchiveHeader;
    entry->bytes = raw; entry->packed_bytes = packed;
    return 1;
}
static inline int diting_adsp_archive_inflate(const struct DitingAdspArchive *entry,
    uint8_t *data, alloc_func allocate, free_func release, voidpf context)
{
    if (!entry || !data || !entry->packed || !entry->bytes ||
        entry->bytes > DitingAdspArchiveLimit || !entry->packed_bytes ||
        entry->packed_bytes > DitingAdspArchiveLimit || !allocate || !release) return 0;
    z_stream stream; memset(&stream, 0, sizeof(stream));
    stream.zalloc = allocate; stream.zfree = release; stream.opaque = context;
    stream.next_in = (Bytef *)(uintptr_t)entry->packed;
    stream.avail_in = (uInt)entry->packed_bytes;
    stream.next_out = data; stream.avail_out = (uInt)entry->bytes;
    if (inflateInit(&stream) != Z_OK) return 0;
    int result = inflate(&stream, Z_FINISH);
    int complete = result == Z_STREAM_END && stream.total_out == entry->bytes &&
        !stream.avail_in && !stream.avail_out;
    int ended = inflateEnd(&stream);
    return complete && ended == Z_OK;
}
#endif
