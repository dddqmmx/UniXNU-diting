/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Diting command-db/LLCC policy. Hardware facts: Nothing SM8475
 * c402fcdb, dcvs/dcvs_fp.c and llcc_qcom.c; captured cape-llcc v2.1 DT.
 * Like Sheng's command-db helpers, this layer performs no hidden I/O. */
#ifndef DITING_MEMORY_POLICY_H
#define DITING_MEMORY_POLICY_H
#include <stddef.h>
#include <stdint.h>

namespace DitingMemory {
constexpr uint32_t DdrMaxKhz = 3196000, LlccMaxKhz = 1066000;
/* Verified diting live.dtb /soc/ddr-freq-table and /soc/llcc-freq-table tiers:
 * Idle    = 547 MHz DDR / 300 MHz LLCC (screen off / deep idle)
 * Display = 768 MHz DDR / 466 MHz LLCC (screen on, idle UI)
 * Active  = 1708 MHz DDR / 806 MHz LLCC (normal UI / touch / light GPU)
 * Max     = 3196 MHz DDR / 1066 MHz LLCC (heavy GPU / 120fps / burst) */
enum Profile : unsigned {
    ProfileIdle = 0,
    ProfileDisplay = 1,
    ProfileActive = 2,
    ProfileMax = 3,
    ProfileCount = 4
};
struct BandwidthProfile {
    uint32_t ddrKhz;
    uint32_t llccKhz;
};
constexpr BandwidthProfile Profiles[ProfileCount] = {
    {547000, 300000},
    {768000, 466000},
    {1708000, 806000},
    {3196000, 1066000},
};
constexpr unsigned Banks = 4, GpuHtwScid = 11, GpuScid = 12;
constexpr unsigned BankOffsets[Banks] = {0, 0x400000, 0x100000, 0x500000};
constexpr unsigned CommonStatus = 0x3400c, WriteScid = 0x21f20;
constexpr unsigned WriteCacheable = 0x21f2c, DisableCapacity = 0x21f00, Retain = 0x21f04;
constexpr uint32_t Active = 1, Inactive = 2;
struct Bcm { uint32_t address, unit, width, vcd; };
inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | uint16_t(p[1]) << 8); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16; }
inline bool fits(size_t at, size_t n, size_t bytes) { return at <= bytes && n <= bytes - at; }
inline bool readBcm(const uint8_t *db, size_t bytes, const char *name, Bcm &out) {
    if (!db || !name || bytes != 0x20000 || !le32(db) || le32(db + 4) != 0x0c0330db) return false;
    uint8_t id[8] = {};
    unsigned len = 0;
    while (len < 8 && name[len]) { id[len] = uint8_t(name[len]); ++len; }
    if (!len || (len == 8 && name[len])) return false;
    size_t headers[8] = {}, sizes[8] = {}, aux[8] = {};
    unsigned rows = 0;
    for (unsigned i = 0; i < 8; ++i) {
        const uint8_t *r = db + 8 + i * 16;
        if (!le16(r)) break;
        headers[i] = 144u + le16(r + 2); sizes[i] = size_t(le16(r + 6)) * 24;
        aux[i] = 144u + le16(r + 4);
        if (!fits(headers[i], sizes[i], bytes) || !fits(aux[i], 0, bytes)) return false;
        ++rows;
    }
    bool found = false;
    Bcm result = {};
    for (unsigned i = 0; i < rows; ++i) for (size_t at = headers[i]; at < headers[i] + sizes[i]; at += 24) {
        const uint8_t *e = db + at;
        bool same = true;
        for (unsigned j = 0; j < 8; ++j) same &= e[j] == id[j];
        if (!same) continue;
        const size_t start = aux[i] + le16(e + 22);
        const uint32_t address = le32(e + 16);
        if (found || le16(db + 8 + i * 16) != 5 || !address || (address & 3) ||
            ((address >> 16) & 7) != 5 || le16(e + 20) != 8 || !fits(start, 8, bytes)) return false;
        for (unsigned j = 0; j < rows; ++j)
            if (start < headers[j] + sizes[j] && headers[j] < start + 8) return false;
        const uint8_t *data = db + start;
        result = {address, le32(data), le16(data + 4), data[6]};
        if (data[7] || result.unit < 1000 || result.unit % 1000 || !result.width) return false;
        found = true;
    }
    if (found) out = result;
    return found;
}
inline bool frequencyVote(const Bcm &bcm, uint32_t khz, uint32_t ceiling, uint32_t &word) {
    if (!bcm.address || (bcm.address & 3) || ((bcm.address >> 16) & 7) != 5 ||
        bcm.unit < 1000 || bcm.unit % 1000 || !bcm.width || !khz || khz > ceiling) return false;
    // The CPU fast-path BCM consumes frequency in kHz, unlike the GPU's
    // bandwidth BCM. dcvs_fp.c divides the command-db unit by 1000 first.
    const uint64_t peak = uint64_t(khz) * bcm.width / (bcm.unit / 1000);
    if (!peak || peak > 0x3fff) return false;
    word = 0x60000000 | uint32_t(peak); // valid, commit, average=0
    return true;
}
inline bool profileVotes(const Bcm &ddrBcm, const Bcm &llccBcm, unsigned profile,
                         uint32_t &ddrKhz, uint32_t &llccKhz, uint32_t &ddrWord, uint32_t &llccWord) {
    if (profile >= ProfileCount) return false;
    ddrKhz = Profiles[profile].ddrKhz;
    llccKhz = Profiles[profile].llccKhz;
    return frequencyVote(ddrBcm, ddrKhz, DdrMaxKhz, ddrWord) &&
           frequencyVote(llccBcm, llccKhz, LlccMaxKhz, llccWord);
}
constexpr unsigned statusReg(unsigned id) { return id * 0x1000 + 4; }
constexpr unsigned attr0Reg(unsigned id) { return 0x21000 + id * 8; }
constexpr unsigned attr1Reg(unsigned id) { return attr0Reg(id) + 4; }
constexpr uint32_t attr1(unsigned kib) { return ((kib * 1024 / Banks / 64) << 16) | 0x18; }
constexpr uint32_t gpuScache1() {
    return GpuScid | GpuScid << 5 | GpuScid << 10 | GpuScid << 15 | GpuScid << 20 | GpuScid << 25;
}
constexpr uint32_t gpuScache0() { return GpuScid << 10 | 1u << 8; }
struct CacheIo {
    void *cookie;
    uint32_t (*readBank)(void *, unsigned, unsigned);
    uint32_t (*readBroadcast)(void *, unsigned);
    void (*writeBroadcast)(void *, unsigned, uint32_t);
    uint64_t (*nowUs)(void *);
    void (*delayUs)(void *, unsigned);
};
enum class CacheResult { Ok, Argument, ForeignActive, Unavailable, Readback, Timeout };
inline bool allBanks(const CacheIo &io, unsigned reg, uint32_t mask, uint32_t value) {
    for (unsigned bank = 0; bank < Banks; ++bank)
        if ((io.readBank(io.cookie, bank, reg) & mask) != value) return false;
    return true;
}
inline CacheResult configureSlice(const CacheIo &io, unsigned id, unsigned kib, bool writes) {
    if (!io.readBank || !io.readBroadcast || !io.writeBroadcast || !io.nowUs || !io.delayUs ||
        (id != GpuScid && id != GpuHtwScid)) return CacheResult::Argument;
    const uint32_t wanted0 = 0xffff0000, wanted1 = attr1(kib), bit = 1u << id;
    const bool configured = allBanks(io, attr0Reg(id), ~0u, wanted0) && allBanks(io, attr1Reg(id), ~0u, wanted1);
    const bool inactive = allBanks(io, statusReg(id), Inactive, Inactive);
    const bool active = allBanks(io, statusReg(id), Active | Inactive, Active);
    // Never rewrite a live firmware/other client's slice. Zero/contradictory
    // state is not proof of stopped ownership or successful activation.
    if (!inactive && !active) return CacheResult::Unavailable;
    if (!inactive && !configured) return CacheResult::ForeignActive;
    if (!configured) {
        io.writeBroadcast(io.cookie, attr1Reg(id), wanted1);
        io.writeBroadcast(io.cookie, attr0Reg(id), wanted0);
        if (!allBanks(io, attr0Reg(id), ~0u, wanted0) || !allBanks(io, attr1Reg(id), ~0u, wanted1)) return CacheResult::Readback;
    }
    const unsigned regs[] = {WriteScid, WriteCacheable, DisableCapacity, Retain};
    const bool set[] = {writes, false, false, true};
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t before = io.readBroadcast(io.cookie, regs[i]);
        const uint32_t value = (before & ~bit) | (set[i] ? bit : 0);
        if (value != before) {
            // The configuration belongs to this inactive slice only. Preserve
            // all CPU/DPU/audio slice bits and inherited active configurations.
            if (!inactive) return CacheResult::ForeignActive;
            io.writeBroadcast(io.cookie, regs[i], value);
        }
        if (!allBanks(io, regs[i], bit, set[i] ? bit : 0)) return CacheResult::Readback;
    }
    if (!active) {
        io.writeBroadcast(io.cookie, id * 0x1000, 3); // ACTIVATE opcode, trigger
        io.writeBroadcast(io.cookie, id * 0x1000, 2); // clear trigger, preserve opcode
        const uint64_t began = io.nowUs(io.cookie);
        for (unsigned i = 0; i < 100 && io.nowUs(io.cookie) - began <= 100; ++i) {
            if (allBanks(io, statusReg(id), Active | Inactive, Active)) return CacheResult::Ok;
            io.delayUs(io.cookie, 1);
        }
        return CacheResult::Timeout;
    }
    return CacheResult::Ok;
}
}
#endif
