// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef DITING_PROBE_H
#define DITING_PROBE_H
#include <stdint.h>

// Optional probes report a result and return. A FAIL must never publish a
// successful hardware resource or touch later MMIO stages that need it.
enum DitingProbeState : unsigned {
  DitingProbePass = 1, DitingProbeFail = 2, DitingProbeSkip = 3, DitingProbeRunning = 4
};

#ifdef DITING_PROBE_IMPLEMENTATION
#define DITING_PROBE_LINK
#elif defined(__APPLE__)
#define DITING_PROBE_LINK __attribute__((weak_import))
#else
#define DITING_PROBE_LINK __attribute__((weak))
#endif
extern "C" void diting_probe_record(unsigned, const char *, unsigned, const char *, uint64_t) DITING_PROBE_LINK;
extern "C" void diting_probe_panic_snapshot(void (*)(const char *, unsigned)) DITING_PROBE_LINK;
#undef DITING_PROBE_LINK

static inline void DitingProbeRecord(unsigned slot, const char *name,
                                   DitingProbeState state, const char *reason,
                                   uint64_t detail = 0)
{
#ifdef DITING_PROBE_IMPLEMENTATION
  diting_probe_record(slot, name, state, reason, detail);
#else
  if (&diting_probe_record) diting_probe_record(slot, name, state, reason, detail);
#endif
}
#endif
