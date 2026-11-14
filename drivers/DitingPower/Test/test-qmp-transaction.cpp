/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../DitingQmpTransaction.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
using namespace DitingQmp;

struct Hardware {
    uint32_t ram[RamBytes / 4] = {};
    uint64_t clock = 0;
    unsigned kicks = 0;
    bool reply = true, handshake = true, late = false;
    std::vector<unsigned> writes;
    Hardware(bool inherited = true) {
        ram[0] = 0x4d41494c; ram[1] = 1; ram[13] = 104; ram[14] = 0x100;
        ram[3] = Up;
        if (inherited) for (unsigned i : {4u, 5u, 6u, 9u, 10u, 11u, 12u}) ram[i] = Up;
    }
    static uint32_t read(void *p, unsigned off) { assert(!(off & 3) && off < RamBytes); return static_cast<Hardware *>(p)->ram[off / 4]; }
    static void write(void *p, unsigned off, uint32_t value) {
        auto h = static_cast<Hardware *>(p); assert(!(off & 3) && off < RamBytes);
        h->writes.push_back(off); h->ram[off / 4] = value;
    }
    static void kick(void *p) { static_cast<Hardware *>(p)->kicks++; }
    static uint64_t now(void *p) { return static_cast<Hardware *>(p)->clock; }
    static void delay(void *p, unsigned us) {
        auto h = static_cast<Hardware *>(p); h->clock += us;
        if (h->handshake) {
            if (h->ram[9] == Up) h->ram[10] = Up;
            if (h->ram[11] == Up) h->ram[5] = Up;
            if (h->ram[6] == Up) h->ram[12] = Up;
        }
        if (h->reply) h->ram[0x100 / 4] = 0;
    }
    static void record(void *p, const char *phase, uint32_t, uint32_t) {
        if (!strcmp(phase, "late-complete")) static_cast<Hardware *>(p)->late = true;
    }
    Io io() { return {this, read, write, kick, now, delay, record}; }
};
int main() {
    const char *wlan = "{class: wlan_pdc, ss: rf, res: s3e.e, upval: 1}";
    {
        Hardware h; Controller c;
        assert(c.send(h.io(), wlan, 64) == Result::Ok);
        assert(h.kicks == 1 && !memcmp(&h.ram[0x104 / 4], wlan, strlen(wlan)));
        for (unsigned i = strlen(wlan); i < 64; ++i) assert(reinterpret_cast<uint8_t *>(h.ram)[0x104 + i] == 0);
        assert(c.send(h.io(), "{class: image, res: load_state, name: adsp, val: on}", 100) == Result::Ok);
        assert(h.kicks == 2);
    }
    {
        Hardware h(false); Controller c;
        assert(c.send(h.io(), wlan, 64) == Result::Ok && h.kicks == 4);
        assert(h.ram[4] == Up && h.ram[6] == Up);
    }
    {
        Hardware h; Controller c; h.reply = false;
        assert(c.send(h.io(), wlan, 64) == Result::SendTimeout);
        assert(h.ram[0x100 / 4] == 64);
        const size_t writes = h.writes.size();
        assert(c.send(h.io(), "different request", 100) == Result::Busy && h.writes.size() == writes);
        h.ram[0x100 / 4] = 0; h.reply = true;
        assert(c.send(h.io(), "different request", 100) == Result::Ok && h.late);
    }
    {
        Hardware h; Controller c; h.ram[0x100 / 4] = 64;
        assert(c.send(h.io(), wlan, 64) == Result::Busy && h.writes.empty());
    }
    {
        Hardware h; Controller c; h.ram[14] = 0x3f0;
        assert(c.send(h.io(), wlan, 64) == Result::Descriptor && h.writes.empty());
    }
    {
        Hardware h(false); Controller c; h.handshake = false;
        assert(c.send(h.io(), wlan, 64) == Result::LinkTimeout);
        assert(h.ram[0x100 / 4] == 0);
        h.handshake = true;
        assert(c.send(h.io(), wlan, 64) == Result::Ok);
    }
    {
        Hardware h; Controller c; char tooLong[65]; memset(tooLong, 'a', 64); tooLong[64] = 0;
        assert(c.send(h.io(), tooLong, 64) == Result::Argument);
        assert(c.send(h.io(), nullptr, 64) == Result::Argument);
        assert(c.send(h.io(), wlan, 65) == Result::Argument && h.writes.empty());
    }
    {
        Hardware h; Controller c;
        assert(c.send(h.io(), wlan, 64) == Result::Ok);
        const size_t writes = h.writes.size(); h.ram[14] += 4;
        assert(c.send(h.io(), wlan, 64) == Result::Descriptor && h.writes.size() == writes);
    }
    puts("PASS: 8 QMP ownership, wire and timeout scenarios");
}
