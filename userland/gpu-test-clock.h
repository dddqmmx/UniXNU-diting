/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

namespace DitingGpuPacing {
constexpr uint64_t PeriodNs = 1000000000ull / 120;
// Preserve a full input/observation period after a delayed wake. Sending all
// expired coordinates at once changes the gesture being measured.
static inline uint64_t next(uint64_t origin, unsigned step, uint64_t previous) {
    const uint64_t planned = origin + uint64_t(step) * PeriodNs;
    const uint64_t spaced = previous + PeriodNs;
    return planned > spaced ? planned : spaced;
}
}
