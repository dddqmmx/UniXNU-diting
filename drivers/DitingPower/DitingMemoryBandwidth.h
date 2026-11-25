/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_MEMORY_BANDWIDTH_H
#define DITING_MEMORY_BANDWIDTH_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void diting_memory_bandwidth_start(void);
int diting_memory_gpu_cache(void);
/* Adjusts DDR/LLCC bandwidth profile (0=Idle, 1=Display, 2=Active, 3=Max)
 * when diting_memory_bw=1 (dynamic mode). No-op when disabled or locked Max. */
bool diting_memory_bandwidth_set_profile(unsigned level);
unsigned diting_memory_bandwidth_profile(void);
unsigned diting_memory_snapshot_text(char *out, unsigned capacity);
unsigned diting_memory_bandwidth_probe_text(char *out, unsigned capacity);
// Diagnostic fields are requests/raw registers, never an asserted DDR rate.
struct DitingMemoryDiagnostic {
    uint32_t version, bytes, sequence, stage;
    int32_t error, cache_result;
    uint32_t enabled, cache_ready;
    uint64_t us;
    uint32_t bcm[2][6]; // address, unit, width, vcd, requested_kHz, wire word
    uint32_t common_status[4], slice[2][4][3]; // status, attr0, attr1 per bank
    uint32_t shared[4]; // WRSC, cacheable WRSC, disabled-capacity, retention
    uint32_t bwmon[2][10]; // enable, window, byte count, timer, zones, max[4], IRQ
    uint32_t bwmon_mapped;
};
extern volatile struct DitingMemoryDiagnostic diting_memory_diagnostic;
#ifdef __cplusplus
}
#endif
#endif
