/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host-only model of posted control writes and independent completion.
 * Exercise failure/ownership cases without sending any hardware votes. */
#include "../DitingRpmhTransaction.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
using namespace DitingRpmh;

struct Hardware {
    uint32_t regs[0x10000 / 4] = {};
    uint64_t clock = 0, finishAt = 0;
    unsigned running = TotalTcs, triggers = 0;
    enum Behavior { Normal, NoProgress, Stuck, Partial } behavior = Normal;
    bool armAcknowledged = false, dropArm = false, lateRecorded = false;
    std::vector<std::pair<unsigned, uint32_t>> writes;
    Hardware() {
        regs[0] = 2u << 16;
        regs[0xc / 4] = (8u << 12) | (16u << 27);
        for (unsigned s = 0; s < TotalTcs; ++s) {
            regs[(TcsOffset + s * TcsStride + 0x18) / 4] = 1;
            regs[(TcsOffset + s * TcsStride + 0x3c) / 4] = IssuedComplete;
        }
    }
    void finish() {
        if (running == TotalTcs) return;
        unsigned b = TcsOffset + running * TcsStride;
        uint32_t mask = regs[(b + 0x1c) / 4];
        for (unsigned i = 0; i < 16; ++i) if (mask & (1u << i))
            regs[(b + 0x3c + i * CmdStride) / 4] = behavior == Partial && i ? 0 : IssuedComplete;
        regs[(b + 0x18) / 4] = 1;
        running = TotalTcs;
    }
    static uint32_t read(void *p, unsigned off) {
        auto &h = *static_cast<Hardware *>(p);
        assert(off < sizeof h.regs && !(off & 3));
        if (h.running != TotalTcs && h.behavior != Stuck && h.clock >= h.finishAt) h.finish();
        if (off >= TcsOffset && (off - TcsOffset) % TcsStride == 0x14 && h.regs[off / 4] == AmcEnable)
            h.armAcknowledged = true;
        return h.regs[off / 4];
    }
    static void write(void *p, unsigned off, uint32_t v) {
        auto &h = *static_cast<Hardware *>(p);
        assert(off < sizeof h.regs && !(off & 3));
        h.writes.emplace_back(off, v);
        if (off == TcsOffset + 8) { h.regs[(TcsOffset + 4) / 4] &= ~v; return; }
        if (off >= TcsOffset + 0x10) {
            unsigned s = (off - TcsOffset) / TcsStride, sub = (off - TcsOffset) % TcsStride;
            // Any write into the sleep/wake/fast TCS banks is a regression.
            assert(s < 3);
            if (sub == 0x14) {
                uint32_t old = h.regs[off / 4];
                if ((old & Trigger) && !(v & Trigger)) assert(v & AmcEnable);
                if (h.dropArm && v == AmcEnable) return;
                if (v & Trigger) {
                    assert(h.armAcknowledged && (old & AmcEnable));
                    h.armAcknowledged = false; ++h.triggers;
                    if (h.behavior != NoProgress) {
                        h.running = s; h.finishAt = h.clock + 30;
                        unsigned b = TcsOffset + s * TcsStride;
                        h.regs[(b + 0x18) / 4] = 0;
                        uint32_t mask = h.regs[(b + 0x1c) / 4];
                        for (unsigned i = 0; i < 16; ++i) if (mask & (1u << i))
                            h.regs[(b + 0x3c + i * CmdStride) / 4] = 0;
                    }
                }
            }
        }
        h.regs[off / 4] = v;
    }
    static uint64_t now(void *p) { return static_cast<Hardware *>(p)->clock; }
    static void delay(void *p, unsigned us) { static_cast<Hardware *>(p)->clock += us; }
    static void record(void *p, const char *phase, unsigned, uint32_t, uint32_t,
                       uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {
        if (!strcmp(phase, "late-complete")) static_cast<Hardware *>(p)->lateRecorded = true;
    }
    Io io() { return {this, read, write, now, delay, record}; }
};

int main() {
    const uint32_t a[] = {0x30010, 0x30000}, v[] = {8, 9};
    {
        Hardware h; Controller c;
        // No enabled IRQ: a fresh busy/response transition still completes.
        assert(c.vote(h.io(), a, v, 2) == Result::Ok && h.triggers == 1);
        assert(h.regs[(TcsOffset + 0x14) / 4] == 0 && h.regs[(TcsOffset + 0x1c) / 4] == 0);
        assert(c.vote(h.io(), a, v, 1) == Result::Ok && h.triggers == 2);
    }
    {
        Hardware h; Controller c; h.behavior = Hardware::NoProgress;
        // Old idle + ISSUED/COMPL without new progress cannot become an ACK.
        assert(c.vote(h.io(), a, v, 1) == Result::CompletionTimeout);
        auto count = h.writes.size();
        assert(c.vote(h.io(), a + 1, v + 1, 1) == Result::Retained && h.writes.size() == count);
    }
    {
        Hardware h; Controller c; h.behavior = Hardware::Stuck;
        assert(c.vote(h.io(), a, v, 2) == Result::CompletionTimeout);
        auto count = h.writes.size();
        assert(c.vote(h.io(), a, v, 1) == Result::Retained && h.writes.size() == count);
        h.finish(); h.behavior = Hardware::Normal;
        assert(c.vote(h.io(), a, v, 1) == Result::Ok && h.lateRecorded && h.triggers == 2);
    }
    {
        Hardware h; Controller c; h.behavior = Hardware::Partial;
        assert(c.vote(h.io(), a, v, 2) == Result::CompletionTimeout);
        auto count = h.writes.size();
        assert(c.vote(h.io(), a, v, 1) == Result::Retained && h.writes.size() == count);
    }
    {
        Hardware h; Controller c;
        h.regs[(TcsOffset + 0x1c) / 4] = 1;
        h.regs[(TcsOffset + 0x34) / 4] = 0x30020;
        h.regs[(TcsOffset + 0x18) / 4] = 0;
        assert(c.vote(h.io(), a, v, 1) == Result::Ok);
        assert(h.regs[(TcsOffset + 0x1c) / 4] == 1);
        for (auto w : h.writes) assert(w.first < TcsOffset + 0x10 || w.first >= TcsOffset + TcsStride);
    }
    {
        Hardware h; Controller c;
        h.regs[(TcsOffset + 0x1c) / 4] = 1;
        h.regs[(TcsOffset + 0x34) / 4] = 0x41608;
        const uint32_t vrm = 0x41600, mv = 2800;
        assert(c.vote(h.io(), &vrm, &mv, 1) == Result::Conflict && h.writes.empty());
        assert(sameResource(0x41600, 0x4160c) && !sameResource(0x41600, 0x41700));
    }
    {
        Hardware h; Controller c;
        for (unsigned s = 0; s < 3; ++s) h.regs[(TcsOffset + s * TcsStride + 0x18) / 4] = 0;
        assert(c.vote(h.io(), a, v, 1) == Result::NoFreeTcs && h.writes.empty());
    }
    {
        Hardware h; Controller c; h.dropArm = true;
        assert(c.vote(h.io(), a, v, 1) == Result::ControlTimeout && h.triggers == 0);
        auto count = h.writes.size();
        h.dropArm = false;
        assert(c.vote(h.io(), a, v, 1) == Result::Retained && h.writes.size() == count);
    }
    {
        Hardware h; Controller c; h.regs[0] = 3u << 16;
        assert(c.vote(h.io(), a, v, 1) == Result::Layout && h.writes.empty());
        h.regs[0] = 2u << 16; h.regs[0xc / 4] = (8u << 12) | (1u << 27);
        assert(c.vote(h.io(), a, v, 2) == Result::Layout && h.writes.empty());
    }
    {
        Hardware h; Controller c; const uint32_t bad = 1;
        assert(c.vote(h.io(), &bad, v, 1) == Result::Argument && h.writes.empty());
        assert(c.vote(h.io(), a, v, 0) == Result::Argument && h.writes.empty());
        assert(c.vote(h.io(), a, v, 17) == Result::Argument && h.writes.empty());
    }
    puts("PASS: 10 RPMh scenarios; fresh/partial/late response, posted control ACK, timeout ownership, VRM conflict and sleep/wake preservation");
}
