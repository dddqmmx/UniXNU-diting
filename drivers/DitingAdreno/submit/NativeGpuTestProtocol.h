/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* Fixed diagnostics, separate from the rendering ABI. No path, shader, shell
 * command, address, clock or voltage is supplied by the USB host. */
enum {
    DitingGpuTestVersion = 1, DitingGpuTestPoll = 0x7110,
    DitingGpuTestReport = 0x7111, DitingGpuTestMark = 0x7112, DitingGpuTestReadMode = 0x7113,
    DitingGpuTestChunkBytes = 1024, DitingGpuTestResultBytes = 1048576,
    DitingGpuTestQualification = 1, DitingGpuTestPerformance = 2, DitingGpuTestAll = 3,
    DitingGpuTestUI = 4, DitingGpuTestKeyboard = 5, DitingGpuTestOpenType = 0x4756,
    DitingGpuTestAuto = 0, DitingGpuTestSysmem = 1, DitingGpuTestGmem = 2,
    DitingGpuTestIdle = 0, DitingGpuTestPending = 1, DitingGpuTestRunning = 2,
    DitingGpuTestPassed = 3, DitingGpuTestFailed = 4, DitingGpuTestCancelled = 5,
    DitingGpuTestTimedOut = 6, DitingGpuTestOverflow = 7,
    DitingGpuTestCancel = 1, DitingGpuTestTraceEvent = 64,
    DitingGpuTestDaemonLost = 0x10001
};
struct DitingGpuTestHello { uint32_t version, reserved; };
struct DitingGpuTestJob {
    uint32_t version, job, profile, policy, timeout_ms;
    uint32_t width, height, frames, warmup, flags;
    uint64_t created_us;
};
struct DitingGpuTestUpdate {
    uint32_t version, job, offset, bytes, state, error, case_id, stage;
    uint64_t worker_us;
    uint8_t data[DitingGpuTestChunkBytes];
};
struct DitingGpuTestMarker {
    uint32_t version, job, case_id, phase;
    uint64_t start_ns, end_ns;
};
#if defined(__cplusplus)
static_assert(sizeof(DitingGpuTestHello) == 8, "GPU test hello ABI");
static_assert(sizeof(DitingGpuTestJob) == 48, "GPU test job ABI");
static_assert(sizeof(DitingGpuTestUpdate) == 1064, "GPU test report ABI");
static_assert(sizeof(DitingGpuTestMarker) == 32, "GPU test marker ABI");
#else
_Static_assert(sizeof(struct DitingGpuTestHello) == 8, "GPU test hello ABI");
_Static_assert(sizeof(struct DitingGpuTestJob) == 48, "GPU test job ABI");
_Static_assert(sizeof(struct DitingGpuTestUpdate) == 1064, "GPU test report ABI");
_Static_assert(sizeof(struct DitingGpuTestMarker) == 32, "GPU test marker ABI");
#endif
