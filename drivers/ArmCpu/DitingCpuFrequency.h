/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Read-only EPSS evidence. PERF_STATE is a request, not a measured clock. */
struct DitingCpuFrequencyDomain {
    uint32_t first, count, live, levels, enabled, per_core;
    uint32_t domain_state, limit_status, pdmem_index, highest_khz;
    uint32_t request[4], request_khz[4];
};
struct DitingCpuFrequencySnapshot {
    uint32_t version, bytes, sequence, mode;
    uint64_t timestamp_ns;
    struct DitingCpuFrequencyDomain domains[3];
};
#if defined(__cplusplus)
static_assert(sizeof(DitingCpuFrequencySnapshot)==240,"CPU frequency snapshot ABI");
#endif
