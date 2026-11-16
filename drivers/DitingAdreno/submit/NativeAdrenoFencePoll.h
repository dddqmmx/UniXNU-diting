/* SPDX-License-Identifier: MIT */
#pragma once
#include "NativeAdrenoRing.h"

static inline uint32_t native_adreno_wait_us(uint64_t ns) {
    const uint64_t us = ns / 1000 + (ns % 1000 != 0);
    return us > UINT32_MAX ? UINT32_MAX : uint32_t(us);
}
// poll owns the shared gate only for a zero-timeout observation. idle is always
// outside that gate, so unrelated clients can submit and retire during a wait.
template<typename Poll>
static inline IOReturn native_adreno_poll_fence(const NativeAdrenoRingClock &clock,
    uint32_t timeout_us, Poll poll) {
    if (!clock.now_us) return kIOReturnBadArgument;
    const uint64_t began = clock.now_us(clock.cookie);
    for (;;) {
        const IOReturn result = poll();
        if (result != kIOReturnTimeout || clock.now_us(clock.cookie) - began >= timeout_us) return result;
        if (!clock.idle || clock.idle(clock.cookie)) return kIOReturnTimeout;
    }
}
