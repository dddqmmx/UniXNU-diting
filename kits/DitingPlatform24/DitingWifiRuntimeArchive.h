/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_WIFI_RUNTIME_ARCHIVE_H
#define DITING_WIFI_RUNTIME_ARCHIVE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Five ordered zlib streams: bootstrap, its job, wifid job, new native
 * cache, exact captured cache preimage. Destinations live in the installer;
 * the archive contains no paths, network preferences or credentials. */
enum { DitingWifiRuntimeFiles = 5, DitingWifiRuntimeHeader = 48,
       DitingWifiRuntimePackedMax = 4u << 20 };
struct DitingWifiRuntimeEntry {
    const uint8_t *packed;
    uint64_t bytes, packed_bytes;
};
static inline uint64_t diting_wifi_runtime_u64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8*i);
    return value;
}
static inline uint32_t diting_wifi_runtime_u32(const uint8_t *p)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (8*i);
    return value;
}
static inline int diting_wifi_runtime_parse(const uint8_t *data, size_t bytes,
    struct DitingWifiRuntimeEntry entries[DitingWifiRuntimeFiles])
{
    static const uint64_t limits[DitingWifiRuntimeFiles] = {1u<<20,65536,65536,2u<<20,2u<<20};
    if (!data || !entries || bytes < 168 || bytes > DitingWifiRuntimePackedMax ||
        memcmp(data,"DTWIFI01",8) || diting_wifi_runtime_u32(data+8)!=1 ||
        diting_wifi_runtime_u32(data+12)!=0x8475 ||
        diting_wifi_runtime_u32(data+16)!=DitingWifiRuntimeFiles ||
        diting_wifi_runtime_u32(data+20) || diting_wifi_runtime_u64(data+24)!=bytes) return 0;
    unsigned id = 0;
    for (unsigned i=32; i<DitingWifiRuntimeHeader; ++i) id |= data[i];
    if (!id) return 0;
    size_t cursor = DitingWifiRuntimeHeader;
    for (unsigned i=0; i<DitingWifiRuntimeFiles; ++i) {
        if (bytes-cursor < 16) return 0;
        uint64_t raw = diting_wifi_runtime_u64(data+cursor);
        uint64_t packed = diting_wifi_runtime_u64(data+cursor+8);
        cursor += 16;
        if (raw<8 || raw>limits[i] || !packed || packed>DitingWifiRuntimePackedMax) return 0;
        uint64_t padded = (packed+7) & ~UINT64_C(7);
        if (padded>bytes-cursor) return 0;
        for (uint64_t n=packed; n<padded; ++n) if (data[cursor+n]) return 0;
        entries[i].packed=data+cursor; entries[i].bytes=raw; entries[i].packed_bytes=packed;
        cursor += (size_t)padded;
    }
    return cursor==bytes;
}
#endif
