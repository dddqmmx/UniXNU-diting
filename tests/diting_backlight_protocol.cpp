/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "DitingBacklightProtocol.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
using namespace DitingBrightness;

struct Hardware {
    std::array<uint32_t, 256> registers = {};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    std::vector<uint32_t> fifo;
    unsigned resets = 0;
    Hardware() {
        registers[Version / 4] = 0x20060000;
        registers[Control / 4] = 3; // inherited DSI + video, command engine off
        registers[DmaControl / 4] = 0xa9000100;
        registers[DmaLength / 4] = 0x1234;
        registers[TriggerControl / 4] = 0x18105;
        registers[PatternControl / 4] = 0x1200;
        registers[InterruptControl / 4] = EnableMask | (1u << 16) | (1u << 28);
    }
    static uint32_t read(void *context, uint32_t offset) {
        assert(offset < 0x400 && !(offset & 3));
        return static_cast<Hardware *>(context)->registers[offset / 4];
    }
    static void write(void *context, uint32_t offset, uint32_t value) {
        auto &h = *static_cast<Hardware *>(context);
        assert(offset < 0x400 && !(offset & 3));
        h.writes.emplace_back(offset, value);
        if (offset == InterruptControl) {
            constexpr uint32_t status = DmaDone | BtaDone | ErrorPending | (1u << 8) |
                (1u << 10) | (1u << 12) | (1u << 14) | (1u << 16) | (1u << 28) | (1u << 30);
            assert(!(value & status & ~(DmaDone | BtaDone | ErrorPending)));
            h.registers[offset / 4] = (h.registers[offset / 4] & ~(value & status) & ~EnableMask) |
                (value & EnableMask);
        } else if (offset == AckError) h.registers[offset / 4] &= ~value;
        else {
            h.registers[offset / 4] = value;
            if (offset == PatternData) h.fifo.push_back(value);
            if (offset == DmaTrigger) h.registers[Status / 4] |= DmaBusy;
            if (offset == FifoReset && value) { ++h.resets; h.fifo.clear(); }
            if (offset == ReadControl && value) {
                h.registers[ReadControl / 4] = 0;
                h.registers[ReadData0 / 4] = h.registers[ReadData1 / 4] = 0;
            }
        }
    }
    static void delay(void *, unsigned us) { assert(us == 1); }
    Io io() { return {this, read, write, delay}; }
    void complete(bool query = false) {
        registers[Status / 4] &= ~DmaBusy;
        registers[InterruptControl / 4] |= DmaDone | (query ? BtaDone : 0);
    }
};

static const Board board = {0xae94000, 0x400, 4095, 1, 1, 0, 1};
static void ranges() {
    assert(validBoard(board));
    auto old = board; old.minimum = 10; assert(!validBoard(old));
    old = board; old.base = 0xae96000; assert(!validBoard(old));
    old = board; old.channel = 1; assert(!validBoard(old));
    old = board; old.version = 2; assert(!validBoard(old));
    assert(requestedLevel(board, 0, 32) == 0);
    assert(requestedLevel(board, 1, 32) == 1);
    assert(requestedLevel(board, 4095, 32) == 4095);
    assert(requestedLevel(board, 6000, 32) == 4095);
    assert(requestedLevel(board, 0xffffffff, 32) == 0);
    assert(requestedLevel(board, 0xffffffffffffffffULL, 64) == 0);
    assert(requestedLevel(board, 0x100000000ULL, 64) == 4095);
    assert(fadeMilliseconds(0x8000, 32) == 500);
    assert(fadeMilliseconds(6u << 16, 32) == 5000);
    assert(fadeMilliseconds(0xffffffff, 32) == 0);
    assert(fadeMilliseconds(UINT64_MAX, 64) == 0);
    assert(fadeLevel(1000, 2000, 0, 500000000) == 1000);
    assert(fadeLevel(1000, 2000, 250000000, 500000000) == 1500);
    assert(fadeLevel(2000, 0, 250000000, 500000000) == 1000);
    assert(fadeLevel(2000, 0, 500000000, 500000000) == 0);
    assert(fadeLevel(1, 4095, UINT64_MAX, 500000000) == 4095);
}
static void wire() {
    for (uint32_t level = 0; level <= 4095; ++level) {
        const uint32_t word = writePayload(level);
        assert((word & 0xff) == 0x51);
        assert(((word >> 8) & 0xff) == (level >> 8));
        assert(((word >> 16) & 0xff) == (level & 0xff));
        uint32_t raw = ~0u;
        const uint32_t reply = 0x22000000u | ((level >> 8) << 16) | ((level & 0xff) << 8);
        assert(readLevel(4, reply, 0, raw) && raw == level);
        uint32_t measured = ~0u;
        const uint32_t native = level * 4u;
        const uint32_t get = 0x22000000u | ((native >> 8) << 16) | ((native & 0xff) << 8);
        assert(readLevel(4, get, 0, raw) && raw == native);
        assert(controlLevel(board, raw, measured) && measured == level);
    }
    uint32_t raw = 0;
    assert(readLevel(8, 0x07ffbeef, 0x1c020000, raw) && raw == 2047);
    assert(readLevel(4, 0x22191000, 0, raw) && raw == 6416); // preserve physical GET units
    for (const auto &observed : std::array<std::array<uint32_t, 2>, 6>{{
        {0, 0}, {1, 4}, {1604, 6416}, {787, 3148}, {3894, 15576}, {4045, 16180}}}) {
        uint32_t measured = ~0u;
        assert(controlLevel(board, observed[1], measured) && measured == observed[0]);
    }
    uint32_t measured = 0;
    assert(controlLevel(board, 8192, measured) && measured == 2048);
    assert(controlLevel(board, 16383, measured) && measured == 4095);
    assert(!controlLevel(board, 16384, measured));
    assert(!controlLevel(board, UINT32_MAX, measured));
    for (const auto &reply : std::vector<std::array<uint32_t, 3>>{
        {0, 0x22000100, 0}, {3, 0x22000100, 0}, {7, 0x22000100, 0}, {20, 0x22000100, 0},
        {4, 0x21010000, 0}, {4, 0x62000100, 0}, {4, 0x02000000, 0},
        {8, 0x01000000, 0x1c030000}, {8, 0x01000000, 0x5c020000}})
        assert(!readLevel(reply[0], reply[1], reply[2], raw));
}
static void packets() {
    Hardware h; Command command; command.initialize(h.io());
    const auto saved = h.registers;
    const uint32_t words[] = {WriteBrightnessHeader, writePayload(2047)};
    assert(command.begin(words, 2, false, false, 0));
    assert(h.fifo == std::vector<uint32_t>({0xc0390003, 0xffff0751}));
    assert(h.registers[DmaLength / 4] == 8);
    assert(!(h.registers[DmaControl / 4] & (1u << 26)));
    assert((h.registers[TriggerControl / 4] & 0x1000f) == 4);
    assert(command.poll(1000) == Completion::Pending);
    h.complete(); assert(command.poll(1001) == Completion::Done); assert(command.finish());
    for (unsigned reg : {Control, DmaControl, DmaLength, TriggerControl, PatternControl})
        assert(h.registers[reg / 4] == saved[reg / 4]);
    assert(h.registers[InterruptControl / 4] == saved[InterruptControl / 4]);
    assert(h.resets == 1);
    const uint32_t size[] = {ReturnSizePacket};
    assert(command.begin(size, 1, true, false, 2000));
    assert(h.fifo == std::vector<uint32_t>({0x80370002, 0}));
    assert(h.registers[DmaLength / 4] == 4);
    assert(h.registers[DmaControl / 4] & (1u << 26));
}
static void busy() {
    for (auto failure : std::vector<std::pair<unsigned, uint32_t>>{
        {Status, DmaBusy}, {Status, MdpBusy}, {Control, 0}, {PatternControl, 1},
        {PatternControl, 6}, {InterruptControl, ErrorPending}, {AckError, 1u << 20}}) {
        Hardware h; h.registers[failure.first / 4] = failure.second;
        h.registers[ReadControl / 4] = 4u << 16; h.registers[ReadData0 / 4] = 0x22010000;
        Command command; command.initialize(h.io());
        const auto saved = h.registers;
        const uint32_t query[] = {ReadBrightnessPacket};
        assert(!command.begin(query, 1, true, true, 0));
        assert(h.writes.empty() && h.registers == saved);
    }
    Hardware h; h.registers[Status / 4] = 1u << 3; // running video does not block commands
    Command command; command.initialize(h.io()); const uint32_t size[] = {ReturnSizePacket};
    assert(!command.begin(nullptr, 1, true, false, 0));
    assert(!command.begin(size, Command::MaximumWords+1, true, false, 0));
    assert(h.writes.empty()); assert(command.begin(size, 1, true, false, 0));
}
static void retained() {
    Hardware h; Command command; command.initialize(h.io());
    const uint32_t size[] = {ReturnSizePacket}; assert(command.begin(size, 1, true, false, 0));
    const auto writes = h.writes.size();
    assert(command.poll(50000000) == Completion::Retained);
    assert(!command.finish() && h.writes.size() == writes && !h.resets);
    assert(!command.begin(size, 1, true, false, 60000000));
    h.registers[Status / 4] = 0; // idle without a completion is not retirement
    assert(command.poll(60000000) == Completion::Retained);
    assert(!command.finish() && h.writes.size() == writes);
    h.complete(); assert(command.poll(60000001) == Completion::Done);
    assert(command.finish() && !command.inFlight());
    assert(command.begin(size, 1, true, false, 60000002));
}
static void bta() {
    Hardware h; Command command; command.initialize(h.io());
    const uint32_t query[] = {ReadBrightnessPacket}; assert(command.begin(query, 1, true, true, 0));
    assert(h.fifo == std::vector<uint32_t>({0xa0060052, 0}));
    h.complete(); const auto writes = h.writes.size();
    assert(command.poll(1000) == Completion::Pending);
    assert(command.poll(50000000) == Completion::Retained);
    assert(!command.finish() && h.writes.size() == writes);
    h.complete(true); h.registers[ReadControl / 4] = 4u << 16;
    h.registers[ReadData0 / 4] = 0x2207ff00;
    assert(command.poll(50000001) == Completion::Done);
    uint32_t raw = 0;
    assert(readLevel(h.registers[ReadControl / 4] >> 16, h.registers[ReadData0 / 4], 0, raw) && raw == 2047);
    assert(command.finish());
}
static void errors() {
    for (bool foreign : {false, true}) {
        Hardware h; Command command; command.initialize(h.io());
        const uint32_t query[] = {ReadBrightnessPacket}; assert(command.begin(query, 1, true, true, 0));
        h.registers[AckError / 4] = 1u << 17;
        h.registers[InterruptControl / 4] |= ErrorPending;
        if (foreign) h.registers[ClockStatus / 4] = 1u << 16;
        assert(command.poll(1000) == Completion::Pending); // error does not prove retirement
        h.complete(true); assert(command.poll(1001) == Completion::Error);
        assert(command.finish()); assert(!h.registers[AckError / 4]);
        bool sawAckClearPair = false;
        for (size_t i = 0; i + 1 < h.writes.size(); ++i) {
            if (h.writes[i].first == AckError && h.writes[i].second == (1u << 17) &&
                h.writes[i + 1].first == AckError && h.writes[i + 1].second == 0) {
                sawAckClearPair = true;
                break;
            }
        }
        assert(sawAckClearPair);
        assert(bool(h.registers[InterruptControl / 4] & ErrorPending) == foreign);
        assert(command.available() != foreign);
        if (foreign) assert(h.registers[ClockStatus / 4] == (1u << 16));
        for (auto write : h.writes) assert(write.first != ClockStatus);
    }
}
static void latchedAck() {
    struct LatchedHardware : Hardware {
        uint32_t armedClear = 0;
        static void writeLatched(void *context, uint32_t offset, uint32_t value) {
            auto &h = *static_cast<LatchedHardware *>(context);
            if (offset == AckError) {
                h.writes.emplace_back(offset, value);
                if (value != 0) h.armedClear |= value;
                else { h.registers[AckError / 4] &= ~h.armedClear; h.armedClear = 0; }
                return;
            }
            Hardware::write(context, offset, value);
        }
        Io latchedIo() { return {this, read, writeLatched, delay}; }
    };
    LatchedHardware h; Command command; command.initialize(h.latchedIo());
    const uint32_t query[] = {ReadBrightnessPacket};
    assert(command.begin(query, 1, true, true, 0));
    h.registers[AckError / 4] = 0x01000002u; // observed SoT Sync + bit 24
    h.registers[InterruptControl / 4] |= ErrorPending;
    h.complete(true);
    assert(command.poll(1001) == Completion::Error);
    assert(command.finish());
    assert((h.registers[AckError / 4] & AckErrorMask) == 0);
    assert(!(h.registers[InterruptControl / 4] & ErrorPending));
    assert(command.available());
}
static void wakePolicy() {
    // 1. Live failure sequence: boot 307 -> req#1 1280 -> sleep -> req#2 0 -> wake restores 1280.
    WakePolicy p;
    p.onInitialLevel(board, 307, 37 << 16);
    assert(p.initialized && p.lastNonZeroLevel == 307 && p.currentTarget == 307);
    auto a = p.onBrightnessRequest(board, 1280, 156 << 16, true);
    assert(a.submit && !a.restored && a.target == 1280);
    assert(p.lastNonZeroLevel == 1280 && p.lastNonZeroNits == (156 << 16));
    a = p.onFramebufferState(board, false);
    assert(a.submit && !a.restored && a.target == 0 && !p.fbPowered);
    assert(p.lastNonZeroLevel == 1280);
    // CoreBrightness sends brightness-nits=0 during sleep: must NOT clobber cached non-zero level.
    a = p.onBrightnessRequest(board, 0, 0, true);
    assert(a.submit && !a.restored && a.target == 0);
    assert(p.lastNonZeroLevel == 1280 && p.lastNonZeroNits == (156 << 16));
    // Duplicate sleep is a no-op.
    a = p.onFramebufferState(board, false);
    assert(!a.submit);
    // Framebuffer wake restores cached level 1280.
    a = p.onFramebufferState(board, true);
    assert(a.submit && a.restored && a.target == 1280 && p.fbPowered && p.currentTarget == 1280);
    // Duplicate wake is a no-op.
    a = p.onFramebufferState(board, true);
    assert(!a.submit);

    // 2. Preserve user's lowest non-zero slider choice (level=1) across sleep/wake.
    a = p.onBrightnessRequest(board, 1, 2 << 16, true);
    assert(a.submit && a.target == 1 && p.lastNonZeroLevel == 1);
    a = p.onFramebufferState(board, false);
    assert(a.submit && a.target == 0);
    a = p.onFramebufferState(board, true);
    assert(a.submit && a.restored && a.target == 1);

    // 3. EnableBacklight=false blocks wake until EnableBacklight=true arrives.
    a = p.onEnableRequest(board, false);
    assert(a.submit && a.target == 0 && !p.enabled && p.lastNonZeroLevel == 1);
    a = p.onFramebufferState(board, false);
    assert(!a.submit && !p.fbPowered);
    a = p.onFramebufferState(board, true);
    assert(!a.submit && p.fbPowered && p.currentTarget == 0);
    a = p.onEnableRequest(board, true);
    assert(a.submit && a.restored && a.target == 1 && p.enabled);

    // 4. Brightness-nits=0 while awake followed by EnableBacklight=true restores cached brightness.
    a = p.onBrightnessRequest(board, 1500, 180 << 16, true);
    assert(a.target == 1500);
    a = p.onBrightnessRequest(board, 0, 0, true);
    assert(a.target == 0 && p.lastNonZeroLevel == 1500);
    a = p.onEnableRequest(board, true);
    assert(a.submit && a.restored && a.target == 1500);

    // 5. Explicit non-zero brightness during sleep is not overwritten on subsequent wake.
    a = p.onFramebufferState(board, false);
    assert(a.submit && a.target == 0);
    a = p.onBrightnessRequest(board, 900, 110 << 16, true);
    assert(a.submit && a.target == 900);
    a = p.onFramebufferState(board, true);
    assert(!a.submit && p.currentTarget == 900);

    // 6. Boot-only sleep/wake (before any setProperties) restores boot level.
    WakePolicy bootOnly;
    bootOnly.onInitialLevel(board, 307);
    a = bootOnly.onFramebufferState(board, false);
    assert(a.submit && a.target == 0);
    a = bootOnly.onFramebufferState(board, true);
    assert(a.submit && a.restored && a.target == 307);
}
static void retryError() {
    assert(shouldRetryError(true, false, 1280, 307, true));
    assert(shouldRetryError(true, false, 0, 307, true));
    assert(!shouldRetryError(false, false, 1280, 307, true));
    assert(!shouldRetryError(true, true, 1280, 307, true));
    assert(!shouldRetryError(true, false, 1280, 307, false));
    assert(!shouldRetryError(true, false, 307, 307, true));
}
static void displayOwner() {
    Hardware h; Command command; command.initialize(h.io());
    const uint32_t size[] = {ReturnSizePacket}; assert(command.begin(size, 1, true, false, 0));
    h.registers[Control / 4] &= ~1u; // display owner turned DSI off during the command
    h.registers[TriggerControl / 4] |= 1u << 22;
    h.registers[DmaControl / 4] |= 1u << 3;
    h.complete(); assert(command.finish());
    assert(!(h.registers[Control / 4] & 1u));
    assert(h.registers[TriggerControl / 4] & (1u << 22));
    assert(h.registers[DmaControl / 4] & (1u << 3));
    assert(!command.available());
    Hardware mdp; Command concurrent; concurrent.initialize(mdp.io());
    assert(concurrent.begin(size, 1, true, false, 0));
    mdp.complete(); mdp.registers[Status / 4] |= MdpBusy;
    assert(concurrent.finish());
    assert(mdp.registers[Control / 4] & (1u << 2)); // do not disable an active pixel engine
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    const std::string name = argv[1];
    if (name == "board-file") {
        assert(argc == 3); Board input = {};
        std::ifstream file(argv[2], std::ios::binary); file.read(reinterpret_cast<char *>(&input), sizeof(input));
        assert(file.gcount() == static_cast<std::streamsize>(sizeof(input)) && file.peek() == EOF && validBoard(input));
    } else if (name == "ranges") ranges();
    else if (name == "wire") wire();
    else if (name == "packets") packets();
    else if (name == "busy") busy();
    else if (name == "retained") retained();
    else if (name == "bta") bta();
    else if (name == "errors") errors();
    else if (name == "latched-ack") latchedAck();
    else if (name == "wake-policy") wakePolicy();
    else if (name == "retry-error") retryError();
    else if (name == "display-owner") displayOwner();
    else assert(false);
    std::printf("PASS %s\n", name.c_str());
}
