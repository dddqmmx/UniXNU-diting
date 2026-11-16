/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <string.h>
#include "NativeGpuTestProtocol.h"

struct DitingGpuTestCommand {
    enum Op { Invalid, Status, Run, Cancel, Result };
    Op op = Invalid;
    uint32_t profile = 0, policy = 0, job = 0, offset = 0, bytes = 0;
    static bool decimal(const char *word, uint32_t &value) {
        if (!word || !*word) return false;
        value = 0;
        for (; *word; ++word) {
            if (*word < '0' || *word > '9' || value > (UINT32_MAX - unsigned(*word - '0')) / 10) return false;
            value = value * 10 + unsigned(*word - '0');
        }
        return true;
    }
    static DitingGpuTestCommand parse(const char *text) {
        DitingGpuTestCommand result;
        if (!text) return result;
        const size_t length = strnlen(text, 64);
        if (!length || length >= 64 || text[length - 1] == ' ') return result;
        char line[64]; memcpy(line, text, length + 1);
        char *words[5]{}; unsigned count = 0;
        for (char *p = line; *p;) {
            if (count == 5 || *p == ' ') return result;
            words[count++] = p;
            while (*p && *p != ' ') {
                if (*p < 33 || *p > 126) return result;
                ++p;
            }
            if (*p) *p++ = 0;
        }
        if (count < 2 || strcmp(words[0], "gputest")) return result;
        if (count == 2 && !strcmp(words[1], "status")) result.op = Status;
        if (count == 4 && !strcmp(words[1], "run")) {
            result.profile = !strcmp(words[2], "qualification") ? DitingGpuTestQualification :
                !strcmp(words[2], "performance") ? DitingGpuTestPerformance :
                !strcmp(words[2], "all") ? DitingGpuTestAll :
                !strcmp(words[2], "ui") ? DitingGpuTestUI : 0;
            result.policy = !strcmp(words[3], "auto") ? DitingGpuTestAuto :
                !strcmp(words[3], "sysmem") ? DitingGpuTestSysmem :
                !strcmp(words[3], "gmem") ? DitingGpuTestGmem : 99;
            if (result.profile && result.policy <= DitingGpuTestGmem &&
                (result.profile != DitingGpuTestUI || result.policy == DitingGpuTestAuto)) result.op = Run;
        }
        if (count == 3 && !strcmp(words[1], "cancel") && decimal(words[2], result.job) && result.job) result.op = Cancel;
        if (count == 5 && !strcmp(words[1], "result") && decimal(words[2], result.job) && result.job &&
            decimal(words[3], result.offset) && decimal(words[4], result.bytes) &&
            result.offset <= DitingGpuTestResultBytes && result.bytes && result.bytes <= DitingGpuTestChunkBytes)
            result.op = Result;
        return result;
    }
};
