/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Polled SM8475 APSS DRV2 active-only transactions. The captured Diting DT
 * lists <ACTIVE 3>, <SLEEP 2>, <WAKE 2>, <CONTROL 0>, <FAST 1>, in that order.
 * Thus active TCSes are 0..2; 3..4 must remain available to the sleep solver.
 * Register ordering follows Linux drivers/soc/qcom/rpmh-rsc.c. The caller
 * serializes every APSS client for the whole submission/completion/reclaim.
 * No IOKit, allocations, locks or hardware access are hidden in this layer.
 */
#ifndef DITING_RPMH_TRANSACTION_H
#define DITING_RPMH_TRANSACTION_H
#include <stdint.h>

namespace DitingRpmh {
constexpr unsigned TcsOffset = 0xd00, TcsStride = 0x2a0, CmdStride = 20;
constexpr unsigned ActiveMask = 7, TotalTcs = 8, MaxCommands = 16;
constexpr uint32_t Trigger = 1u << 24, AmcEnable = 1u << 16;
constexpr uint32_t IssuedComplete = 0x10100, WriteResponse = 0x10108;

enum class Result {
    Ok, Argument, Layout, NoFreeTcs, Conflict, Retained,
    ControlTimeout, CompletionTimeout, ReclaimTimeout
};
inline const char *reason(Result r) {
    switch (r) {
    case Result::Ok: return "complete";
    case Result::Argument: return "invalid request";
    case Result::Layout: return "unsupported APSS DRV2 layout";
    case Result::NoFreeTcs: return "no demonstrably free active TCS";
    case Result::Conflict: return "foreign request for the same resource";
    case Result::Retained: return "unfinished transaction still owns controller";
    case Result::ControlTimeout: return "control write acknowledgement timeout";
    case Result::CompletionTimeout: return "response timeout; transaction retained";
    case Result::ReclaimTimeout: return "reclaim timeout; transaction retained";
    }
    return "unknown result";
}

struct Io {
    void *cookie;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t);
    uint64_t (*nowUs)(void *);
    void (*delayUs)(void *, unsigned);
    void (*record)(void *, const char *, unsigned, uint32_t, uint32_t,
                   uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
};

inline bool sameResource(uint32_t a, uint32_t b) {
    // VRM voltage/enable/mode are offsets within ONE accelerator resource.
    return a == b || (((a >> 16) & 7) == 4 && (a & ~15u) == (b & ~15u));
}

class Controller {
    unsigned tcs = TotalTcs, count = 0;
    bool pending = false, progressed = false, poisoned = false;
    uint32_t address = 0, value = 0;

    unsigned base() const { return TcsOffset + tcs * TcsStride; }
    uint32_t rd(const Io &io, unsigned off) const { return io.read(io.cookie, off); }
    void wr(const Io &io, unsigned off, uint32_t v) const { io.write(io.cookie, off, v); }
    void record(const Io &io, const char *phase) const {
        if (!io.record || tcs >= TotalTcs) return;
        const unsigned b = base();
        io.record(io.cookie, phase, tcs, address, value, rd(io, b + 0x3c),
                  count > 1 ? rd(io, b + 0x3c + CmdStride) : 0,
                  rd(io, b + 0x14), rd(io, b + 0x18), rd(io, TcsOffset + 4));
    }
    bool poll(const Io &io, unsigned off, uint32_t mask, uint32_t wanted) const {
        const uint64_t start = io.nowUs(io.cookie);
        for (unsigned i = 0; i < 500 && io.nowUs(io.cookie) - start < 500; ++i) {
            if ((rd(io, off) & mask) == wanted) return true;
            io.delayUs(io.cookie, 1);
        }
        return false;
    }
    bool sync(const Io &io, unsigned off, uint32_t v) const {
        wr(io, off, v);
        return poll(io, off, ~0u, v);
    }
    bool untrigger(const Io &io) const {
        const unsigned off = base() + 0x14;
        uint32_t v = rd(io, off) & ~Trigger;
        // HW requires separate acknowledged clears, in this order.
        return sync(io, off, v) && sync(io, off, v & ~AmcEnable);
    }
    bool complete(const Io &io) {
        bool commands = true;
        for (unsigned i = 0; i < count; ++i)
            commands &= (rd(io, base() + 0x3c + CmdStride * i) & IssuedComplete) == IssuedComplete;
        const bool idle = (rd(io, base() + 0x18) & 1) != 0;
        if (!commands || !idle) progressed = true;
        // Old ISSUED/COMPL and idle registers are NOT this request's ACK.
        return commands && idle && (progressed || (rd(io, TcsOffset + 4) & (1u << tcs)));
    }
    Result reclaim(const Io &io) {
        if (!untrigger(io) || !sync(io, base() + 0x1c, 0)) {
            poisoned = true;
            record(io, "reclaim-timeout");
            return Result::ReclaimTimeout;
        }
        wr(io, TcsOffset + 8, 1u << tcs);
        if (!poll(io, TcsOffset + 4, 1u << tcs, 0)) {
            poisoned = true;
            record(io, "completion-clear-timeout");
            return Result::ReclaimTimeout;
        }
        pending = false;
        return Result::Ok;
    }

public:
    Result vote(const Io &io, const uint32_t *addresses, const uint32_t *values, unsigned n) {
        if (!addresses || !values || !n || n > MaxCommands) return Result::Argument;
        for (unsigned i = 0; i < n; ++i)
            if (!addresses[i] || (addresses[i] & 3)) return Result::Argument;
        if (poisoned) return Result::Retained;
        if (pending) {
            // Keep software ownership after timeout, even if HW later goes
            // idle. Only THIS request's full response permits its reclamation.
            if (!complete(io)) { record(io, "retained-busy"); return Result::Retained; }
            record(io, "late-complete");
            const Result r = reclaim(io);
            if (r != Result::Ok) return r;
        }
        const unsigned major = (rd(io, 0) >> 16) & 255;
        const uint32_t caps = rd(io, 0xc);
        const unsigned total = (caps >> 12) & 63, ncpt = (caps >> 27) & 31;
        if (!major || major >= 3 || total < TotalTcs || ncpt < n || ncpt > MaxCommands)
            return Result::Layout;
        // All XNU APSS clients use this controller under the same lock.
        // Preserve any inherited/foreign active request instead of overwriting
        // a busy slot. Never inspect/reprogram sleep/wake/fast-path slots.
        for (unsigned s = 0; s < TotalTcs; ++s) if (ActiveMask & (1u << s)) {
            const unsigned b = TcsOffset + s * TcsStride;
            const uint32_t enabled = rd(io, b + 0x1c);
            if (!enabled) continue;
            for (unsigned c = 0; c < ncpt; ++c) if (enabled & (1u << c))
                for (unsigned i = 0; i < n; ++i)
                    if (sameResource(addresses[i], rd(io, b + 0x34 + c * CmdStride)))
                        return Result::Conflict;
        }
        tcs = TotalTcs;
        for (unsigned s = 0; s < TotalTcs; ++s) if (ActiveMask & (1u << s)) {
            const unsigned b = TcsOffset + s * TcsStride;
            if ((rd(io, b + 0x18) & 1) && !rd(io, b + 0x1c) && !(rd(io, b + 0x14) & Trigger)) {
                tcs = s; break;
            }
        }
        if (tcs == TotalTcs) return Result::NoFreeTcs;
        count = n; address = addresses[0]; value = values[0]; progressed = false;
        record(io, "prepare");
        wr(io, TcsOffset, rd(io, TcsOffset) & ~(1u << tcs));
        if (!untrigger(io) || !sync(io, base() + 0x1c, 0)) {
            poisoned = true; record(io, "control-timeout"); return Result::ControlTimeout;
        }
        wr(io, TcsOffset + 8, 1u << tcs);
        if (!poll(io, TcsOffset + 4, 1u << tcs, 0)) {
            poisoned = true; record(io, "stale-completion-timeout"); return Result::ControlTimeout;
        }
        for (unsigned i = 0; i < n; ++i) {
            const unsigned off = base() + i * CmdStride;
            wr(io, off + 0x30, WriteResponse);
            wr(io, off + 0x34, addresses[i]); wr(io, off + 0x38, values[i]);
        }
        const uint32_t mask = (1u << n) - 1;
        if (!sync(io, base() + 0x10, mask) || !sync(io, base() + 0x1c, mask) ||
            !sync(io, base() + 0x14, AmcEnable)) {
            poisoned = true; record(io, "arm-timeout"); return Result::ControlTimeout;
        }
        pending = true;
        record(io, "submit");
        wr(io, base() + 0x14, AmcEnable | Trigger);
        const uint64_t begin = io.nowUs(io.cookie);
        for (unsigned i = 0; i < 10000 && io.nowUs(io.cookie) - begin < 100000; ++i) {
            if (complete(io)) {
                record(io, "complete");
                return reclaim(io);
            }
            io.delayUs(io.cookie, 10);
        }
        record(io, "response-timeout");
        return Result::CompletionTimeout;
    }
};
}
#endif
