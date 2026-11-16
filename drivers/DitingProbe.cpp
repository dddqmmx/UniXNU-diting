/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#define DITING_PROBE_IMPLEMENTATION
#include "DitingProbe.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

namespace {
struct Slot { unsigned sequence, state; uint64_t detail; char name[32], reason[128]; };
static Slot slots[16];
template<unsigned N> void put(char (&out)[N], const char *in) {
    unsigned n = 0;
    while (in && n + 1 < N && in[n]) { __atomic_store_n(out + n, in[n], __ATOMIC_RELAXED); ++n; }
    for (; n < N; ++n) __atomic_store_n(out + n, char(0), __ATOMIC_RELAXED);
}
template<unsigned N> void get(char (&out)[N], const char (&in)[N]) {
    for (unsigned n = 0; n < N; ++n) out[n] = __atomic_load_n(in + n, __ATOMIC_RELAXED);
    out[N - 1] = 0;
}
}
extern "C" void diting_probe_record(unsigned index, const char *name, unsigned state,
    const char *reason, uint64_t detail) {
    if (index >= 16 || state < DitingProbePass || state > DitingProbeRunning) return;
    Slot &s = slots[index];
    unsigned seq = __atomic_load_n(&s.sequence, __ATOMIC_ACQUIRE);
    if ((seq & 1) || !__atomic_compare_exchange_n(&s.sequence, &seq, seq + 1, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    put(s.name, name); put(s.reason, reason);
    __atomic_store_n(&s.state, state, __ATOMIC_RELAXED);
    __atomic_store_n(&s.detail, detail, __ATOMIC_RELAXED);
    __atomic_store_n(&s.sequence, seq + 2, __ATOMIC_RELEASE);
}
extern "C" void diting_probe_panic_snapshot(void (*emit)(const char *, unsigned)) {
    if (!emit) return;
    for (unsigned i = 0; i < 16; ++i) for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const Slot &s = slots[i];
        const unsigned seq = __atomic_load_n(&s.sequence, __ATOMIC_ACQUIRE);
        if (!seq || (seq & 1)) break;
        char name[32], reason[128]; get(name, s.name); get(reason, s.reason);
        const unsigned state = __atomic_load_n(&s.state, __ATOMIC_RELAXED);
        const uint64_t detail = __atomic_load_n(&s.detail, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq != __atomic_load_n(&s.sequence, __ATOMIC_RELAXED)) continue;
        const char *label = state == DitingProbePass ? "PASS" : state == DitingProbeFail ? "FAIL" :
            state == DitingProbeRunning ? "RUNNING" : "SKIP";
        char line[240];
        const int n = snprintf(line, sizeof(line), "DITING PROBE %u %s: %s %s detail=%llu\n",
            i, name, label, reason, (unsigned long long)detail);
        if (n > 0) emit(line, unsigned(n) < sizeof(line) ? unsigned(n) : unsigned(sizeof(line) - 1));
        break;
    }
}
