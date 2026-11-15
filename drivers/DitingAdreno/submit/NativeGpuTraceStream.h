/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_GPU_TRACE_STREAM_H
#define NATIVE_GPU_TRACE_STREAM_H
#include "NativeGpuTrace.h"

/* One bounded CDC reply. Sequence gaps remain explicit; these records are
 * CPU/GPU events and never physical panel-latch measurements. */
enum { NativeGpuTraceStreamRecords = 20 };
struct NativeGpuTraceStreamHeader {
    struct NativeGpuTraceHeader trace;
    uint64_t first_available, next, skipped, missing;
};
#ifdef __cplusplus
static_assert(sizeof(NativeGpuTraceStreamHeader) == 64, "GPU stream header ABI");
extern "C" {
#endif
uint32_t native_gpu_trace_range(uint64_t after, uint32_t records, void *out, uint32_t bytes);
#ifdef __cplusplus
}
#endif
#endif
