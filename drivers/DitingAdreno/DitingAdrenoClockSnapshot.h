/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef DITING_ADRENO_CLOCK_SNAPSHOT_H
#define DITING_ADRENO_CLOCK_SNAPSHOT_H
#include <stdint.h>

/* Cape GPUCC, checked against Nothing c402fcdb gpucc-waipio.c and the
 * LUCID_OLE register layout. 0x90a8 is a branch, never an RCG divider.
 * This reader has no writes, clock votes, delays or dynamic allocation. */
enum {
    DitingClockSampleSkip, DitingClockSamplePass,
    DitingClockSampleIo, DitingClockSampleChanged,
    DitingClockSampleRegisters = 19
};
enum {
    DitingClockGx, DitingClockGfx, DitingClockRdvm,
    DitingClockPll0Mode, DitingClockPll0Op, DitingClockPll0Status,
    DitingClockPll0L, DitingClockPll0Alpha, DitingClockPll0User,
    DitingClockPll1Mode, DitingClockPll1Op, DitingClockPll1Status,
    DitingClockPll1L, DitingClockPll1Alpha, DitingClockPll1User,
    DitingClockGmuCmd, DitingClockGmuCfg, DitingClockHubCmd, DitingClockHubCfg
};
struct DitingClockSample {
    uint32_t status, reads, value[DitingClockSampleRegisters];
};
static inline unsigned DitingAdrenoReadClockSample(
    void *cookie, int ready, int (*read32)(void *, uint32_t, uint32_t *),
    struct DitingClockSample *out)
{
    static const uint32_t offsets[DitingClockSampleRegisters] = {
        0x905c, 0x90a8, 0x90c8,
        0, 4, 0xc, 0x10, 0x14, 0x18,
        0x1000, 0x1004, 0x100c, 0x1010, 0x1014, 0x1018,
        0x9318, 0x931c, 0x93ec, 0x93f0
    };
    if (!out) return DitingClockSampleIo;
    out->status = out->reads = 0;
    for (unsigned i = 0; i < DitingClockSampleRegisters; ++i) out->value[i] = 0;
    if (!ready) return out->status;
    if (!read32) return out->status = DitingClockSampleIo;
    uint32_t first[DitingClockSampleRegisters] = {0};
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < DitingClockSampleRegisters; ++i) {
            ++out->reads;
            if (read32(cookie, offsets[i], &out->value[i]))
                return out->status = DitingClockSampleIo;
            /* GX must actually be powered before dependent observations. */
            if (i == DitingClockGx && !(out->value[i] & 0x80000000u))
                return out->status;
            if (!pass) first[i] = out->value[i];
        }
    }
    /* Branch/gate status may change normally. A changing PLL or divider
     * cannot provide a qualified frequency observation. */
    static const unsigned stable[] = {
        DitingClockPll0Mode, DitingClockPll0Op, DitingClockPll0L,
        DitingClockPll0Alpha, DitingClockPll0User,
        DitingClockPll1Mode, DitingClockPll1Op, DitingClockPll1L,
        DitingClockPll1Alpha, DitingClockPll1User,
        DitingClockGmuCfg, DitingClockHubCfg
    };
    for (unsigned i = 0; i < sizeof(stable) / sizeof(stable[0]); ++i)
        if (first[stable[i]] != out->value[stable[i]])
            return out->status = DitingClockSampleChanged;
    return out->status = DitingClockSamplePass;
}
#endif
