/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "DitingDwc3Stop.h"
#include "DitingUsbCommand.h"
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <cstdio>

struct Controller {
    std::map<unsigned, uint32_t> reg;
    std::vector<std::pair<unsigned, uint32_t>> writes;
    unsigned pauses = 0;
    bool halt = true, commandError = false;
    Controller() {
        reg[DWC3_GSNPSID] = 0x33313130; reg[DWC3_GCTL] = 2u << 12;
        reg[DWC3_GHWPARAMS1] = 2u << 15;
        reg[DWC3_DCTL] = DWC3_DCTL_RUN_STOP | DWC3_DCTL_KEEP_CONNECT;
        reg[DWC3_DALEPENA] = 1u | 8u;
        reg[DWC3_DEPCMD(0)] = 6u | (1u << 16);
        reg[DWC3_DEPCMD(3)] = 6u | (4u << 16);
        reg[DWC3_GEVNTSIZ(0)] = reg[DWC3_GEVNTSIZ(1)] = 4096;
        reg[DWC3_GEVNTCOUNT(0)] = 4; reg[DWC3_GEVNTCOUNT(1)] = 8;
        reg[DWC3_GEVNTADRLO(0)] = 0xdead0000;
        reg[DWC3_GEVNTADRLO(1)] = 0xbeef0000;
    }
    uint32_t read(unsigned off) {
        if (off == DWC3_DSTS && halt && !(reg[DWC3_DCTL] & DWC3_DCTL_RUN_STOP) &&
            !reg[DWC3_GEVNTCOUNT(0)] && !reg[DWC3_GEVNTCOUNT(1)]) reg[off] |= DWC3_DSTS_DEVCTRLHLT;
        return reg[off];
    }
    void write(unsigned off, uint32_t value) {
        assert(off >= 0xc100 && off < 0xcc00);
        if (off == DWC3_GEVNTADRLO(0) || off == DWC3_GEVNTADRLO(1))
            assert(read(DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT);
        writes.emplace_back(off, value);
        if (off == DWC3_GEVNTCOUNT(0) || off == DWC3_GEVNTCOUNT(1)) {
            assert(reg[off] >= value); reg[off] -= value;
        } else if (off == DWC3_DEPCMD(0) || off == DWC3_DEPCMD(3)) {
            assert((value & 0x3f) == DWC3_DEPCMD_ENDTRANSFER);
            assert((value >> 16) == (reg[off] >> 16));
            assert(value & (1u << 11));
            reg[off] = commandError ? 1u << 12 : value & ~DWC3_DEPCMD_CMDACT;
            reg[DWC3_GEVNTCOUNT(1)] += 4;
        } else reg[off] = value;
    }
    DitingDwc3StopResult stop() {
        return diting_dwc3_stop_device([this](unsigned o){return read(o);},
            [this](unsigned o,uint32_t v){write(o,v);},[this](unsigned){++pauses;});
    }
};

int main(int argc, char **argv) {
    assert(argc == 2); const std::string test = argv[1];
    if (test == "retire") {
        Controller c; const auto s = c.stop(); assert(!s.error);
        assert(s.buffers == 2 && s.acknowledged == 16 && s.commandErrors == 0);
        assert(s.dsts & DWC3_DSTS_DEVCTRLHLT);
        assert(!c.reg[DWC3_GEVNTADRLO(0)] && !c.reg[DWC3_GEVNTADRLO(1)]);
        unsigned ended = 0;
        for (auto w : c.writes) {
            if (w.first == DWC3_DEPCMD(0) || w.first == DWC3_DEPCMD(3)) ++ended;
            assert(w.first != DWC3_DEPCMD(0));
            if (w.first == DWC3_DCTL) assert(ended == 1);
        }
    } else if (test == "halted") {
        Controller c; c.reg[DWC3_DSTS] = DWC3_DSTS_DEVCTRLHLT;
        c.reg[DWC3_DCTL] = 0; c.reg[DWC3_GSNPSID] = 0x5533330a;
        assert(!c.stop().error);
        for (auto w : c.writes) assert(w.first != DWC3_DEPCMD(0) && w.first != DWC3_DEPCMD(3));
    } else if (test == "prerequisites") {
        for (unsigned which = 0; which < 4; ++which) {
            Controller c;
            if (which == 0) c.reg[DWC3_GSNPSID] = 0xffffffff;
            if (which == 1) c.reg[DWC3_GCTL] = 1u << 12;
            if (which == 2) c.reg[DWC3_GHWPARAMS1] = 0;
            if (which == 3) c.reg[DWC3_GHWPARAMS1] = 33u << 15;
            assert(c.stop().error && c.writes.empty());
        }
    } else if (test == "timeouts") {
        Controller c; c.halt = false; c.commandError = true;
        const auto s = c.stop(); assert(s.error && c.pauses == 251 && s.commandErrors == 1);
        assert(c.reg[DWC3_GEVNTADRLO(0)] == 0xdead0000);
        Controller active; active.reg[DWC3_DEPCMD(0)] |= DWC3_DEPCMD_CMDACT;
        assert(active.stop().error && active.writes.empty());
        Controller activeOther; activeOther.reg[DWC3_DEPCMD(3)] = 1u | DWC3_DEPCMD_CMDACT;
        assert(activeOther.stop().error && activeOther.writes.empty());
        Controller unaligned; unaligned.reg[DWC3_GEVNTCOUNT(0)] = 5;
        assert(unaligned.stop().error && unaligned.reg[DWC3_GEVNTCOUNT(0)] == 5);
    } else if (test == "commands") {
        uint64_t address = 0; unsigned bytes = 0;
        assert(diting_usb_parse_read("read 0xfffffff007000000 512", &address, &bytes));
        assert(address == 0xfffffff007000000 && bytes == 512);
        const char *invalid[] = {"read ", "read 0x 1", "read -1 1", "read +1 1", "read 1 0", "read 1 513",
            "read 10000000000000000 1", "read ffffffffffffffff 1", "read 1 -2", "read 1 2 more", "read 1  2",
            "read 1 999999999999999999999999999999999", "read zzz 1", "read 1", "READ 1 2"};
        for (auto line : invalid) assert(!diting_usb_parse_read(line, &address, &bytes));
        for (unsigned i = 0; i < 20000; ++i) {
            std::string fuzz; unsigned seed = i;
            for (unsigned n = 0; n < i % 64; ++n) {seed = seed * 1664525u + 1013904223u; fuzz += char(1 + (seed >> 24) % 127);}
            if (diting_usb_parse_read(fuzz.c_str(), &address, &bytes)) assert(bytes && bytes <= 512 && address <= UINT64_MAX - bytes);
        }
    } else assert(false);
    std::printf("PASS %s\n", argv[1]);
}
