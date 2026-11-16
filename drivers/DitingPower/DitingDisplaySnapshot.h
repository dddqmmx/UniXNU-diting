/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* All offsets are relative to Diting's mdp_phys/dispcc/DSI0 windows. In
 * particular, DSI0 INTF is 0xae36000, not a Sheng base plus 0x35000. */
namespace DitingDisplay {
struct Board {
    uint64_t mdp, clocks, dsi;
    uint32_t mdpBytes, clockBytes, dsiBytes, version;
};
static_assert(sizeof(Board) == 40, "Diting display timing ADT ABI");
enum Region { Mdp, Clocks, Dsi };
enum Status { Ok, Unavailable, PoweredOff, Changed };
struct Io { void *cookie; bool (*read)(void *, Region, uint32_t, uint32_t *); };
constexpr uint32_t Intf[2] = {0x36000, 0x37000};
constexpr uint32_t Pipe[8] = {0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000};
constexpr uint32_t Ctl[6] = {0x16000, 0x17000, 0x18000, 0x19000, 0x1a000, 0x1b000};
constexpr uint32_t ClockOffsets[7] = {0x9000, 0x80a4, 0x800c, 0xb000, 0x8028, 0x802c, 0x8004};
constexpr uint32_t IntfOffsets[18] = {0x000, 0x004, 0x060, 0x0a8, 0x0ac, 0x0b0, 0x00c, 0x26c,
    0x210, 0x284, 0x288, 0x28c, 0x2ac, 0x2b0, 0x2b4, 0x2b8, 0x298, 0x294};
struct Interface { uint32_t reg[18]{}; uint64_t panelTimestamp = 0, mdpTimestamp = 0; uint32_t stable = 0; };
struct Snapshot {
    Status status = Unavailable;
    uint32_t reads = 0, clock[7]{}, dsiVersion = 0, dsiControl = 0, dsiStatus = 0, dsiPattern = 0;
    uint32_t mdss = 0, mdp = 0, pipe[8][6]{}, ctl[6][3]{};
    Interface intf[2];
};
static inline bool valid(const Board &b) {
    return b.version == 1 && b.mdp == 0xae00000 && b.mdpBytes == 0x84000 &&
        b.clocks == 0xaf00000 && b.clockBytes == 0x20000 && b.dsi == 0xae94000 && b.dsiBytes == 0x400;
}
class Reader {
    Board board_; Io io_; Snapshot &out_;
public:
    Reader(const Board &board, Io io, Snapshot &out) : board_(board), io_(io), out_(out) {}
    bool read(Region region, uint32_t offset, uint32_t &value) {
        if (unsigned(region) > Dsi) return false;
        const uint32_t span = region == Mdp ? board_.mdpBytes : region == Clocks ? board_.clockBytes : board_.dsiBytes;
        if ((offset & 3) || offset > span || span - offset < 4 || out_.reads >= 256 || !io_.read) return false;
        ++out_.reads;
        return io_.read(io_.cookie, region, offset, &value);
    }
    bool timestamp(uint32_t low, uint64_t &value) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            uint32_t high = 0, lo = 0, again = 0;
            if (!read(Mdp, low + 4, high) || !read(Mdp, low, lo) || !read(Mdp, low + 4, again)) return false;
            if (high == again) {value = (uint64_t(high) << 32) | lo; return true;}
        }
        return false;
    }
    bool powered(const uint32_t clock[3]) {
        return clock[0] != ~0u && (clock[0] & (1u << 31)) &&
            (clock[1] & 0x80000001u) == 1 && (clock[2] & 0x80000001u) == 1;
    }
    Status capture(bool full) {
        if (!valid(board_) || !io_.read) return Unavailable;
        for (unsigned i = 0; i < (full ? 7u : 3u); ++i)
            if (!read(Clocks, ClockOffsets[i], out_.clock[i])) return Unavailable;
        // Do not touch DPU or DSI while the MDSS domain or register clocks are off.
        if (!powered(out_.clock)) return PoweredOff;
        if (!read(Dsi, 0, out_.dsiVersion) || !read(Dsi, 4, out_.dsiControl)) return Unavailable;
        if (!out_.dsiVersion || out_.dsiVersion == ~0u || !(out_.dsiControl & 1)) return PoweredOff;
        if (full && (!read(Dsi, 8, out_.dsiStatus) || !read(Dsi, 0x15c, out_.dsiPattern) ||
            !read(Mdp, 0, out_.mdss) || !read(Mdp, 0x1000, out_.mdp))) return Unavailable;
        for (unsigned i = 0; i < 2; ++i) {
            auto &v = out_.intf[i];
            for (unsigned n = 0; n < 18; ++n) {
                if (!full && n != 3 && n != 4 && n != 8 && n != 9 && n != 10 && n != 14 && n != 16) continue;
                if (!read(Mdp, Intf[i] + IntfOffsets[n], v.reg[n])) return Unavailable;
            }
            // Disabled/unsupported timestamps stay unavailable; never enable them for a measurement.
            if (v.reg[8] & 1) {
                if (timestamp(Intf[i] + 0x214, v.panelTimestamp)) v.stable |= 1;
                if (timestamp(Intf[i] + 0x21c, v.mdpTimestamp)) v.stable |= 2;
            }
        }
        if (full) {
            // The SRC_FORMAT word alone does not specify byte order. The
            // matching Xiaomi sde_hw_sspp.c uses SRC_UNPACK_PATTERN at 0x34.
            const uint32_t pipeOffsets[] = {0, 0x14, 0x24, 0x30, 0x38, 0x34};
            for (unsigned i = 0; i < 8; ++i) for (unsigned n = 0; n < 6; ++n)
                if (!read(Mdp, Pipe[i] + pipeOffsets[n], out_.pipe[i][n])) return Unavailable;
            const uint32_t ctlOffsets[] = {0x14, 0x18, 0xf4};
            for (unsigned i = 0; i < 6; ++i) for (unsigned n = 0; n < 3; ++n)
                if (!read(Mdp, Ctl[i] + ctlOffsets[n], out_.ctl[i][n])) return Unavailable;
        }
        uint32_t after[3]{};
        for (unsigned i = 0; i < 3; ++i) if (!read(Clocks, ClockOffsets[i], after[i])) return Unavailable;
        for (unsigned i = 0; i < 3; ++i) if (after[i] != out_.clock[i]) return Changed;
        return powered(after) ? Ok : Changed;
    }
};
static inline Snapshot capture(const Board &board, Io io, bool full) {
    Snapshot out;
    Reader reader(board, io, out); out.status = reader.capture(full);
    return out;
}
}
