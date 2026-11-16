/* SPDX-License-Identifier: MIT */
#pragma once
#include <string.h>
#include "NativeGpuTestProtocol.h"

/* The caller serializes this bounded state with its own diagnostics lock.
 * Tests exercise the same job state with deterministic time and real bytes. */
class DitingGpuTestState {
public:
    enum Error { Ok, Busy, Argument, Unavailable, Stale, Full };
    DitingGpuTestJob request{};
    uint32_t state = DitingGpuTestIdle, error = 0, used = 0;
    uint32_t daemon = 0, case_id = 0, stage = 0, quiescing = 0;
    uint64_t heartbeat_us = 0, completed_us = 0;
    uint8_t result[DitingGpuTestResultBytes]{};

    bool active() const { return state == DitingGpuTestPending || state == DitingGpuTestRunning; }
    static uint64_t age(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }
    void expire(uint64_t now) {
        if (active() && age(now, request.created_us) >= uint64_t(request.timeout_ms) * 1000) {
            quiescing = state == DitingGpuTestRunning;
            state = DitingGpuTestTimedOut; request.flags |= DitingGpuTestCancel; completed_us = now;
        }
    }
    Error poll(uint32_t pid, uint64_t now, DitingGpuTestJob &out) {
        expire(now);
        if (!pid) return Argument;
        if (daemon && daemon != pid) {
            if (age(now, heartbeat_us) < 5000000) return Busy;
            // A replacement daemon first reaps the old job. It must never
            // execute a partially reported job again.
            if (active() || quiescing) {
                state = DitingGpuTestFailed; error = DitingGpuTestDaemonLost;
                request.flags |= DitingGpuTestCancel; completed_us = now;
                quiescing = 0;
            }
        }
        daemon = pid; heartbeat_us = now;
        if (state == DitingGpuTestPending) state = DitingGpuTestRunning;
        out = request; out.version = DitingGpuTestVersion;
        if (!active()) out.flags |= DitingGpuTestCancel;
        return Ok;
    }
    Error start(uint32_t profile, uint32_t policy, uint64_t now) {
        expire(now);
        if (profile < DitingGpuTestQualification || profile > DitingGpuTestUI || policy > DitingGpuTestGmem ||
            (profile == DitingGpuTestUI && policy != DitingGpuTestAuto))
            return Argument;
        if (active() || quiescing) return Busy;
        if (!daemon || age(now, heartbeat_us) >= 5000000) return Unavailable;
        uint32_t next = request.job + 1;
        if (!next) ++next;
        request = {DitingGpuTestVersion, next, profile, policy, 120000, 1220, 2712, 240, 24, 0, now};
        state = DitingGpuTestPending; error = used = case_id = stage = quiescing = 0; completed_us = 0;
        return Ok;
    }
    Error append(uint32_t pid, const DitingGpuTestUpdate &in, uint64_t now) {
        expire(now);
        if (in.version != DitingGpuTestVersion || in.bytes > DitingGpuTestChunkBytes ||
            in.state < DitingGpuTestRunning || in.state > DitingGpuTestOverflow)
            return Argument;
        if ((!active() && !quiescing) || !in.job || in.job != request.job || pid != daemon) return Stale;
        if (in.offset != used) return Stale;
        if (quiescing && (in.bytes || in.state == DitingGpuTestRunning)) return Stale;
        if (in.bytes > sizeof(result) - used) {
            state = DitingGpuTestOverflow; request.flags |= DitingGpuTestCancel; completed_us = now;
            quiescing = 1;
            return Full;
        }
        if (in.bytes) memcpy(result + used, in.data, in.bytes);
        used += in.bytes; case_id = in.case_id; stage = in.stage; error = in.error;
        heartbeat_us = now;
        if (in.state != DitingGpuTestRunning) {
            if (!quiescing) state = in.state;
            quiescing = 0; completed_us = now;
        }
        return Ok;
    }
    Error cancel(uint32_t job, uint64_t now) {
        expire(now);
        if (!job || job != request.job) return Stale;
        if (!active()) return Stale;
        quiescing = state == DitingGpuTestRunning;
        request.flags |= DitingGpuTestCancel; state = DitingGpuTestCancelled; completed_us = now;
        return Ok;
    }
    Error read(uint32_t job, uint32_t at, uint32_t bytes, const uint8_t **out, uint32_t *got) const {
        if (!out || !got || !bytes || bytes > DitingGpuTestChunkBytes || at > used) return Argument;
        if (!job || job != request.job) return Stale;
        *got = bytes < used - at ? bytes : used - at; *out = result + at; return Ok;
    }
};
