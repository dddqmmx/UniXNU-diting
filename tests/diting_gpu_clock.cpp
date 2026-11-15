#include "DitingAdrenoClockSnapshot.h"
#include <cassert>
#include <cstdio>
#include <cstring>

struct Hardware {
    unsigned calls = 0, fail = 0, change = 0;
    uint32_t registers[0xa000 / 4]{};
    Hardware() { registers[0x905c / 4] = 0x80000000; }
};
static int read32(void *cookie, uint32_t offset, uint32_t *out)
{
    auto &hw = *static_cast<Hardware *>(cookie);
    assert(!(offset & 3) && offset < 0xa000);
    ++hw.calls;
    if (hw.fail == hw.calls) return 1;
    if (hw.calls == DitingClockSampleRegisters + 1) {
        if (hw.change == 1) ++hw.registers[0x14 / 4];
        if (hw.change == 2) hw.registers[0x90a8 / 4] ^= 0x80000000;
        if (hw.change == 3) hw.registers[0x905c / 4] = 0;
    }
    *out = hw.registers[offset / 4];
    return 0;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    Hardware hw;
    DitingClockSample sample{};
    if (!std::strcmp(argv[1], "prerequisites")) {
        sample.value[0] = 123;
        assert(DitingAdrenoReadClockSample(&hw, 0, read32, &sample) == DitingClockSampleSkip);
        assert(!hw.calls && !sample.reads && !sample.value[0]);
    } else if (!std::strcmp(argv[1], "off")) {
        hw.registers[0x905c / 4] = 0;
        assert(DitingAdrenoReadClockSample(&hw, 1, read32, &sample) == DitingClockSampleSkip);
        assert(hw.calls == 1);
    } else if (!std::strcmp(argv[1], "stable")) {
        hw.registers[0x10 / 4] = 0x4444002e;
        hw.registers[0x14 / 4] = 0xe000;
        assert(DitingAdrenoReadClockSample(&hw, 1, read32, &sample) == DitingClockSamplePass);
        assert(hw.calls == 2 * DitingClockSampleRegisters);
        assert(sample.value[DitingClockPll0L] == 0x4444002e);
        assert(sample.value[DitingClockPll0Alpha] == 0xe000);
    } else if (!std::strcmp(argv[1], "changed")) {
        hw.change = 1;
        assert(DitingAdrenoReadClockSample(&hw, 1, read32, &sample) == DitingClockSampleChanged);
    } else if (!std::strcmp(argv[1], "gate")) {
        hw.change = 2;
        assert(DitingAdrenoReadClockSample(&hw, 1, read32, &sample) == DitingClockSamplePass);
    } else if (!std::strcmp(argv[1], "powerlost")) {
        hw.change = 3;
        assert(DitingAdrenoReadClockSample(&hw, 1, read32, &sample) == DitingClockSampleSkip);
        assert(hw.calls == DitingClockSampleRegisters + 1);
    } else if (!std::strcmp(argv[1], "io")) {
        hw.fail = 4;
        assert(DitingAdrenoReadClockSample(&hw, 1, read32, &sample) == DitingClockSampleIo);
        assert(hw.calls == 4);
    } else if (!std::strcmp(argv[1], "arguments")) {
        assert(DitingAdrenoReadClockSample(&hw, 1, read32, nullptr) == DitingClockSampleIo);
        assert(!hw.calls);
        assert(DitingAdrenoReadClockSample(&hw, 1, nullptr, &sample) == DitingClockSampleIo);
        assert(!hw.calls);
    } else assert(false);
    std::printf("PASS %s\n", argv[1]);
}
