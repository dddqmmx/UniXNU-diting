/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8475 Diting L12 physical panel 60/90/120 Hz refresh-rate switch protocol.
 * Targets l12_42_02_0a_dsc_cmd (live unit default) and l12_36_02_0b_dsc_cmd.
 * Timing-switch command streams match platform-firmware/diting-live.dts and
 * dts/diting.dts byte-for-byte; clockrate (0x4190ab00) & PHY timings are
 * identical across 60/90/120 Hz, so no PLL reprogram is needed.
 */
#ifndef DITING_PANEL_REFRESH_PROTOCOL_H
#define DITING_PANEL_REFRESH_PROTOCOL_H

#include "DitingDsiTransport.h"
#include "DitingDisplayRefresh.h"
#include <stdint.h>

namespace DitingPanelRefresh {

enum class PanelKind : uint8_t {
    Unknown = 0,
    L12_42_02_0a = 1,
    L12_36_02_0b = 2,
};

enum class RefreshRate : uint32_t {
    Hz60 = 60,
    Hz90 = 90,
    Hz120 = 120,
};

inline bool validRefreshRate(uint32_t hz, RefreshRate &out) {
    if (hz == 60u) { out = RefreshRate::Hz60; return true; }
    if (hz == 90u) { out = RefreshRate::Hz90; return true; }
    if (hz == 120u) { out = RefreshRate::Hz120; return true; }
    return false;
}

inline const char *panelKindName(PanelKind kind) {
    switch (kind) {
    case PanelKind::L12_42_02_0a: return "qcom,mdss_dsi_l12_42_02_0a_dsc_cmd";
    case PanelKind::L12_36_02_0b: return "qcom,mdss_dsi_l12_36_02_0b_dsc_cmd";
    default: return "unknown";
    }
}

/* SM8475 DPU / DISPCC physical timing constants matching DitingDisplaySnapshot.h */
constexpr uint64_t MdpBase = 0x0ae00000ULL;
constexpr uint32_t MdpBytes = 0x84000u;
constexpr uint64_t DispCcBase = 0x0af00000ULL;
constexpr uint32_t DispCcBytes = 0x20000u;
constexpr uint64_t Dsi0Base = 0x0ae94000ULL;
constexpr uint32_t Dsi0Bytes = 0x400u;

constexpr uint32_t IntfOffset[2] = {0x36000u, 0x37000u};
constexpr uint32_t DispCcGdscOff = 0x9000u;
constexpr uint32_t DispCcMdssAhbOff = 0x80a4u;
constexpr uint32_t DispCcMdssMdpOff = 0x800cu;
constexpr uint32_t DispCcByte0Off = 0x8028u;
constexpr uint32_t DispCcPclk0Off = 0x8004u;

constexpr uint32_t IntfTsCtrlOff = 0x210u;
constexpr uint32_t IntfPanelTsLoOff = 0x214u;
constexpr uint32_t IntfPanelTsHiOff = 0x218u;
constexpr uint32_t IntfTeEnOff = 0x284u;
constexpr uint32_t IntfTeCfgOff = 0x288u;
constexpr uint32_t IntfTeHeightOff = 0x28cu;
constexpr uint32_t IntfTeInitOff = 0x294u;
constexpr uint32_t IntfTeCountOff = 0x298u;
constexpr uint32_t IntfTeLineOff = 0x2b0u;
constexpr uint32_t IntfAutorefreshOff = 0x2b4u;

constexpr uint32_t TeCfgVsyncCountMask = 0x7ffffu;
constexpr uint32_t TeCfgVsyncInEn = 1u << 20;
constexpr uint32_t TeCfgVsyncCountEn = 1u << 19;

constexpr uint64_t XoClockHz = 19200000ULL;
/* 1220x2712 + vbp(20) + vfp(20) + vpw(4) = 2756 scanlines */
constexpr uint32_t DitingVTotalLines = 2756u;

inline uint32_t computeVsyncCount(RefreshRate rate, uint32_t vtotal = DitingVTotalLines) {
    const uint32_t hz = (uint32_t)rate;
    if (!hz || !vtotal) return 0u;
    const uint32_t count = (uint32_t)(XoClockHz / ((uint64_t)vtotal * hz));
    return count & TeCfgVsyncCountMask;
}

inline uint32_t updateTeCfgForRate(uint32_t currentTeCfg, RefreshRate rate,
                                   uint32_t vtotal = DitingVTotalLines) {
    const uint32_t preserved = currentTeCfg & ~TeCfgVsyncCountMask;
    return preserved | computeVsyncCount(rate, vtotal);
}

/* Qualcomm DT command item descriptor parsed from DT 7-byte header + payload */
struct DsiCmdEntry {
    uint8_t dt;
    uint8_t last;
    uint8_t vc;
    uint8_t flags; /* 0x40 = batch with following command */
    uint8_t waitMs;
    uint16_t payloadLen;
    const uint8_t *payload;
};

struct TimingSwitchBatch {
    uint32_t words[64];
    uint32_t wordCount;
    uint32_t waitAfterMs;
    bool lowPower;
};

struct TimingSwitchPlan {
    TimingSwitchBatch batches[4];
    uint32_t batchCount;
    uint8_t expectedReg2F;
};

/* Verify whether a single raw DT qcom,mdss-dsi-timing-switch-command blob parses cleanly */
inline bool decodeDtCommandStream(const uint8_t *blob, uint32_t blobBytes,
                                  TimingSwitchPlan &outPlan, bool lowPower, uint8_t expectedReg2F) {
    if (!blob || !blobBytes) return false;
    outPlan = {};
    outPlan.expectedReg2F = expectedReg2F;

    uint32_t pos = 0;
    uint32_t batchIdx = 0;
    bool inBatch = false;

    while (pos < blobBytes) {
        if (pos + 7u > blobBytes) return false;
        const uint8_t dt = blob[pos + 0];
        const uint8_t last = blob[pos + 1];
        const uint8_t vc = blob[pos + 2];
        const uint8_t flags = blob[pos + 3];
        const uint8_t waitMs = blob[pos + 4];
        const uint16_t dlen = ((uint16_t)blob[pos + 5] << 8) | blob[pos + 6];
        pos += 7u;
        if (last > 1u || vc || (flags & ~0x40u) || dlen > blobBytes - pos || dlen == 0u) return false;
        const uint8_t *payload = &blob[pos];
        pos += dlen;

        if (!inBatch) {
            if (batchIdx >= 4u) return false;
            outPlan.batches[batchIdx] = {};
            outPlan.batches[batchIdx].lowPower = lowPower;
            inBatch = true;
        }
        auto &b = outPlan.batches[batchIdx];
        /* Split at wait boundaries or whenever flags bit 0x40 (batch continue) is clear */
        const bool batchContinue = (flags & 0x40u) != 0u && waitMs == 0u && pos < blobBytes;
        const bool lastInDma = !batchContinue;

        if (!DitingDsi::appendEmbeddedPacket(dt, vc, lastInDma, false, payload, dlen,
                                             b.words, 64u, b.wordCount))
            return false;

        if (lastInDma) {
            b.waitAfterMs = waitMs;
            ++batchIdx;
            inBatch = false;
        }
    }
    if (inBatch || !batchIdx) return false;
    outPlan.batchCount = batchIdx;
    return true;
}

/* Exact DT byte sequences verified against platform-firmware/diting-live.dts & dts/diting.dts */
namespace DtStreams {
/* qcom,mdss_dsi_l12_42_02_0a_dsc_cmd (80 bytes each) */
constexpr uint8_t L12_42_60Hz[80] = {
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x02,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x09,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xc0, 0x00,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xcc, 0x30,
    0x15, 0x00, 0x00, 0x00, 0x14, 0x00, 0x02, 0xce, 0x01,
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xcc, 0x00,
};
constexpr uint8_t L12_42_90Hz[80] = {
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x04,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x0b,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xc0, 0x20,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xcc, 0x30,
    0x15, 0x00, 0x00, 0x00, 0x0d, 0x00, 0x02, 0xce, 0x01,
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xcc, 0x00,
};
constexpr uint8_t L12_42_120Hz[80] = {
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x03,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x0a,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xc0, 0x20,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xcc, 0x30,
    0x15, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x02, 0xce, 0x01,
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xcc, 0x00,
};

/* qcom,mdss_dsi_l12_36_02_0b_dsc_cmd (98 bytes each) */
constexpr uint8_t L12_36_60Hz[98] = {
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x00,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x08,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xe0, 0x01,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x05, 0xe2, 0x88, 0xe0, 0xcc, 0x0c,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x12,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x03, 0xe2, 0x86, 0x01,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x09, 0xea, 0x02, 0x00, 0xe0, 0x00, 0xe0, 0x70, 0x00, 0x70,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x17,
    0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xea, 0x80, 0x00, 0x80,
};
constexpr uint8_t L12_36_90Hz[98] = {
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x01,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x08,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xe0, 0x01,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x05, 0xe2, 0x42, 0xee, 0x04, 0x20,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x12,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x03, 0xe2, 0xfd, 0x03,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x09, 0xea, 0x02, 0x00, 0xb8, 0x00, 0xb8, 0x58, 0x00, 0x58,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x17,
    0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xea, 0x48, 0x00, 0x48,
};
constexpr uint8_t L12_36_120Hz[98] = {
    0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x2f, 0x02,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x06, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x08,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0xe0, 0x01,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x05, 0xe2, 0xd6, 0x4e, 0x84, 0x88,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x12,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x03, 0xe2, 0xfd, 0x03,
    0x39, 0x00, 0x00, 0x40, 0x00, 0x00, 0x09, 0xea, 0x02, 0x00, 0xe0, 0x00, 0xe0, 0x80, 0x00, 0x80,
    0x15, 0x00, 0x00, 0x40, 0x00, 0x00, 0x02, 0x6f, 0x17,
    0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xea, 0x38, 0x00, 0x38,
};
} /* namespace DtStreams */

inline bool buildTimingSwitchPlan(PanelKind kind, RefreshRate rate, TimingSwitchPlan &outPlan) {
    /* Both L12 panels specify qcom,mdss-dsi-timing-switch-command-state = "dsi_hs_mode" */
    if (kind == PanelKind::L12_42_02_0a) {
        switch (rate) {
        case RefreshRate::Hz60:
            return decodeDtCommandStream(DtStreams::L12_42_60Hz, sizeof(DtStreams::L12_42_60Hz), outPlan, false, 0x02u);
        case RefreshRate::Hz90:
            return decodeDtCommandStream(DtStreams::L12_42_90Hz, sizeof(DtStreams::L12_42_90Hz), outPlan, false, 0x04u);
        case RefreshRate::Hz120:
            return decodeDtCommandStream(DtStreams::L12_42_120Hz, sizeof(DtStreams::L12_42_120Hz), outPlan, false, 0x03u);
        }
    } else if (kind == PanelKind::L12_36_02_0b) {
        switch (rate) {
        case RefreshRate::Hz60:
            return decodeDtCommandStream(DtStreams::L12_36_60Hz, sizeof(DtStreams::L12_36_60Hz), outPlan, false, 0x00u);
        case RefreshRate::Hz90:
            return decodeDtCommandStream(DtStreams::L12_36_90Hz, sizeof(DtStreams::L12_36_90Hz), outPlan, false, 0x01u);
        case RefreshRate::Hz120:
            return decodeDtCommandStream(DtStreams::L12_36_120Hz, sizeof(DtStreams::L12_36_120Hz), outPlan, false, 0x02u);
        }
    }
    return false;
}

inline PanelKind identifyPanelFromIds(uint8_t da, uint8_t db, uint8_t dc) {
    if (da == 0x42u && db == 0x02u && dc == 0x0au)
        return PanelKind::L12_42_02_0a;
    if (da == 0x36u && db == 0x02u && dc == 0x0bu)
        return PanelKind::L12_36_02_0b;
    return PanelKind::Unknown;
}

inline uint32_t expectedReg2F(PanelKind kind, RefreshRate rate) {
    TimingSwitchPlan plan{};
    return buildTimingSwitchPlan(kind, rate, plan) ? plan.expectedReg2F : 0xffu;
}

/* Read-only / controlled DPU + DISPCC hardware callbacks */
struct DpuIo {
    void *context = nullptr;
    uint32_t (*readMdp)(void *, uint32_t offset) = nullptr;
    void (*writeMdp)(void *, uint32_t offset, uint32_t value) = nullptr;
    uint32_t (*readDispCc)(void *, uint32_t offset) = nullptr;
};

inline bool isDisplayPowered(const DpuIo &dpu, const DitingDsi::Io &dsiIo) {
    if (!dpu.readDispCc || !dpu.readMdp || !dsiIo.read) return false;
    const uint32_t gdsc = dpu.readDispCc(dpu.context, DispCcGdscOff);
    const uint32_t ahb = dpu.readDispCc(dpu.context, DispCcMdssAhbOff);
    const uint32_t mdp = dpu.readDispCc(dpu.context, DispCcMdssMdpOff);
    const uint32_t byte0 = dpu.readDispCc(dpu.context, DispCcByte0Off);
    const uint32_t pclk0 = dpu.readDispCc(dpu.context, DispCcPclk0Off);
    if (!(gdsc & (1u << 31)) || (ahb & 0x80000001u) != 1u || (mdp & 0x80000001u) != 1u)
        return false;
    if ((byte0 & 1u) != 1u || (pclk0 & 1u) != 1u)
        return false;
    const uint32_t dsiVer = dsiIo.read(dsiIo.context, DitingDsi::Version);
    const uint32_t dsiCtrl = dsiIo.read(dsiIo.context, DitingDsi::Control);
    return dsiVer != 0u && dsiVer != ~0u && (dsiCtrl & DitingDsi::ControlEnable) != 0u;
}

inline int findActiveIntf(const DpuIo &dpu) {
    if (!dpu.readMdp) return -1;
    for (int i = 0; i < 2; ++i) {
        const uint32_t base = IntfOffset[i];
        if ((dpu.readMdp(dpu.context, base + IntfTeEnOff) & 1u) &&
            (dpu.readMdp(dpu.context, base + IntfTsCtrlOff) & 1u))
            return i;
    }
    return -1;
}

inline bool readPanelTimestamp(const DpuIo &dpu, uint32_t intfIdx, uint64_t &outTs) {
    if (intfIdx >= 2u || !dpu.readMdp) return false;
    const uint32_t loOff = IntfOffset[intfIdx] + IntfPanelTsLoOff;
    const uint32_t hiOff = IntfOffset[intfIdx] + IntfPanelTsHiOff;
    for (unsigned attempt = 0; attempt < 3u; ++attempt) {
        const uint32_t hi1 = dpu.readMdp(dpu.context, hiOff);
        const uint32_t lo = dpu.readMdp(dpu.context, loOff);
        const uint32_t hi2 = dpu.readMdp(dpu.context, hiOff);
        if (hi1 == hi2) {
            outTs = ((uint64_t)hi1 << 32) | lo;
            return true;
        }
    }
    return false;
}

struct RateMeasurement {
    uint32_t edges = 0;
    uint32_t periodUs = 0;
    uint32_t milliHz = 0;
    uint64_t firstTs = 0;
    uint64_t lastTs = 0;
    bool valid = false;

    void reset() { *this = RateMeasurement{}; }

    void sample(uint64_t ts) {
        if (!ts) return;
        if (!firstTs) {
            firstTs = ts;
            lastTs = ts;
            return;
        }
        if (ts > lastTs) {
            lastTs = ts;
            ++edges;
            if (edges >= 2u && lastTs > firstTs) {
                const uint64_t dtTicks = lastTs - firstTs;
                /* Protect against uint64 overflow while computing us & mHz */
                const uint64_t avgTicksPerEdge = dtTicks / edges;
                if (avgTicksPerEdge > 0u) {
                    periodUs = (uint32_t)((avgTicksPerEdge * 1000000ULL) / XoClockHz);
                    milliHz = (uint32_t)(((uint64_t)edges * XoClockHz * 1000ULL) / dtTicks);
                    valid = true;
                }
            }
        }
    }
};

class Controller {
public:
    enum class State : uint8_t {
        Idle = 0,
        SetReadSize1 = 1,
        ReadIdDa = 2,
        ReadIdDb = 3,
        ReadIdDc = 4,
        ReadModeBefore = 5,
        SendSwitchBatch = 6,
        WaitAfterBatch = 7,
        ReadModeAfter = 8,
        UpdateDpuTe = 9,
        VerifyPanelTs = 10,
        Done = 11,
        Failed = 12,
        ReadAddressMode = 13,
        ReadPixelFormat = 14,
        Quiesce = 15,
    };

    struct Telemetry {
        State state = State::Idle;
        PanelKind panel = PanelKind::Unknown;
        RefreshRate targetRate = RefreshRate::Hz120;
        uint8_t idDa = 0, idDb = 0, idDc = 0;
        uint8_t reg2FBefore = 0xffu;
        uint8_t reg2FAfter = 0xffu;
        int activeIntf = -1;
        uint32_t teCfgBefore = 0;
        uint32_t teCfgAfter = 0;
        uint32_t autorefresh = 0;
        uint32_t batchesSent = 0;
        uint32_t dmaPacketsSent = 0;
        uint32_t tpgPacketsSent = 0;
        uint32_t addressMode = 0xff, pixelFormat = 0xff, colourReadValid = 0, colourReadErrors = 0;
        RateMeasurement teRate;
        const char *error = nullptr;
    };

private:
    DitingDsi::Transport transport_;
    DitingDisplayRefresh::Transition commit_;
    DpuIo dpu_ = {};
    DitingDsi::Io dsiIo_ = {};
    Telemetry info_ = {};
    TimingSwitchPlan plan_ = {};
    uint32_t batchCursor_ = 0;
    uint64_t waitUntilNs_ = 0;
    bool pendingTx_ = false;
    bool expectingRead_ = false;
    uint8_t expectedReadLen_ = 1u;
    bool updateDpuTeCfg_ = true;
    uint32_t targetTeSamples_ = 4u;
    uint32_t teSampleTicks_ = 0;
    PanelKind hintPanel_ = PanelKind::Unknown;

    bool submitSingleWord(uint32_t word, bool lowPower, bool read, uint64_t nowNs) {
        if (!transport_.begin(&word, 1u, lowPower, read, nowNs, false)) return false;
        pendingTx_ = true;
        expectingRead_ = read;
        return true;
    }

    bool startReadReg(uint8_t reg, State nextState, uint64_t nowNs) {
        const uint32_t pkt = DitingDsi::encodeShortWord(DitingDsi::DtDcsRead0, reg, 0u, 0u, true, true);
        if (!submitSingleWord(pkt, true, true, nowNs)) return false;
        expectedReadLen_ = 1u;
        info_.state = nextState;
        return true;
    }

    bool fail(const char *msg) {
        info_.state = State::Failed;
        info_.error = msg;
        // An unretired transport keeps its DMA buffer and DSI ownership.
        // Never turn a timeout into permission to retry or reuse the FIFO.
        pendingTx_ = transport_.inFlight();
        return false;
    }

public:
    void initialize(const DitingDsi::Io &dsiIo, const DitingDsi::DmaBuffer &dma, const DpuIo &dpuIo) {
        dsiIo_ = dsiIo;
        dpu_ = dpuIo;
        transport_.initialize(dsiIo, dma);
        commit_.initialize({dpuIo.context,dpuIo.readMdp,dpuIo.writeMdp,dpuIo.readDispCc},dsiIo);
        info_ = Telemetry{};
    }

    bool start(RefreshRate targetRate, uint64_t nowNs,
               PanelKind hintPanel = PanelKind::Unknown,
               bool updateDpuTeCfg = true,
               uint32_t targetTeEdges = 4u) {
        if (transport_.inFlight() || commit_.retainsOwnership())
            return fail("Previous command/display transition retains ownership");
        if (!isDisplayPowered(dpu_, dsiIo_))
            return fail("Display clocks or DSI0 controller powered off");
        const int intf = findActiveIntf(dpu_);
        if (intf < 0)
            return fail("No active TE/TS DPU INTF found");

        info_ = Telemetry{};
        info_.targetRate = targetRate;
        info_.activeIntf = intf;
        const uint32_t base = IntfOffset[intf];
        info_.teCfgBefore = dpu_.readMdp(dpu_.context, base + IntfTeCfgOff);
        info_.autorefresh = dpu_.readMdp(dpu_.context, base + IntfAutorefreshOff);
        (void)hintPanel; // A name in bootargs is not a measured panel identity.
        hintPanel_ = PanelKind::Unknown;
        updateDpuTeCfg_ = updateDpuTeCfg;
        targetTeSamples_ = targetTeEdges ? targetTeEdges : 1u;
        teSampleTicks_ = 0;
        batchCursor_ = 0;

        /* Always set max return packet size = 1 before 0xDA/0xDB/0xDC/0x2F reads */
        const uint32_t retSize1 = DitingDsi::encodeShortWord(
            DitingDsi::DtSetMaxReturnSize, 1u, 0u, 0u, true, false);
        if (!submitSingleWord(retSize1, true, false, nowNs))
            return fail("DSI0 command engine busy at start");
        info_.state = State::SetReadSize1;
        ++info_.tpgPacketsSent;
        return true;
    }

    /* Advance state machine; returns suggested poll interval ms (0 when finished) */
    uint32_t step(uint64_t nowNs) {
        if (info_.state == State::Done || info_.state == State::Failed || info_.state == State::Idle)
            return 0u;

        if (pendingTx_) {
            const auto comp = transport_.poll(nowNs);
            if (comp == DitingDsi::Completion::Pending) return 1u;
            if (comp == DitingDsi::Completion::Retained) {
                fail("DSI0 transaction timeout");
                return 0u;
            }
            uint8_t rxBytes[4] = {};
            bool rxOk = true;
            if (expectingRead_) {
                const uint32_t rxCount = dsiIo_.read(dsiIo_.context, DitingDsi::ReadControl) >> 16;
                const uint32_t d0 = dsiIo_.read(dsiIo_.context, DitingDsi::ReadData0);
                const uint32_t d1 = dsiIo_.read(dsiIo_.context, DitingDsi::ReadData1);
                rxOk = DitingDsi::decodeReadReply(rxCount, d0, d1, rxBytes, expectedReadLen_, 4u);
            }
            if (!transport_.finish()) {
                fail("DSI0 finish failed");
                return 0u;
            }
            pendingTx_ = false;
            const bool optionalColour = info_.state == State::ReadAddressMode || info_.state == State::ReadPixelFormat;
            if (comp == DitingDsi::Completion::Error && !optionalColour) {
                fail("DSI0 hardware ACK/FIFO/lane error");
                return 0u;
            }
            if (expectingRead_ && !rxOk && !optionalColour) {
                fail("Panel readback packet malformed");
                return 0u;
            }

            switch (info_.state) {
            case State::SetReadSize1:
                if (hintPanel_ != PanelKind::Unknown) {
                    info_.panel = hintPanel_;
                    if (!startReadReg(0x2fu, State::ReadModeBefore, nowNs))
                        { fail("Failed to read 0x2f"); return 0u; }
                    ++info_.tpgPacketsSent;
                    return 1u;
                }
                if (!startReadReg(0xdau, State::ReadIdDa, nowNs))
                    { fail("Failed to read ID1 (0xDA)"); return 0u; }
                ++info_.tpgPacketsSent;
                return 1u;
            case State::ReadIdDa:
                info_.idDa = rxBytes[0];
                if (!startReadReg(0xdbu, State::ReadIdDb, nowNs))
                    { fail("Failed to read ID2 (0xDB)"); return 0u; }
                ++info_.tpgPacketsSent;
                return 1u;
            case State::ReadIdDb:
                info_.idDb = rxBytes[0];
                if (!startReadReg(0xdcu, State::ReadIdDc, nowNs))
                    { fail("Failed to read ID3 (0xDC)"); return 0u; }
                ++info_.tpgPacketsSent;
                return 1u;
            case State::ReadIdDc:
                info_.idDc = rxBytes[0];
                info_.panel = identifyPanelFromIds(info_.idDa, info_.idDb, info_.idDc);
                if (info_.panel == PanelKind::Unknown) {
                    fail("Unrecognized L12 panel ID");
                    return 0u;
                }
                if (!startReadReg(0x2fu, State::ReadModeBefore, nowNs))
                    { fail("Failed to read initial 0x2f"); return 0u; }
                ++info_.tpgPacketsSent;
                return 1u;
            case State::ReadModeBefore:
                info_.reg2FBefore = rxBytes[0];
                if (!buildTimingSwitchPlan(info_.panel, info_.targetRate, plan_)) {
                    fail("Failed to build timing-switch plan");
                    return 0u;
                }
                batchCursor_ = 0;
                if (!startReadReg(0x0bu, State::ReadAddressMode, nowNs))
                    { fail("Failed to read panel address mode"); return 0u; }
                ++info_.tpgPacketsSent;
                return 1u;
            case State::ReadAddressMode:
                if (rxOk && comp == DitingDsi::Completion::Done) {
                    info_.addressMode = rxBytes[0]; info_.colourReadValid |= 1;
                } else info_.colourReadErrors |= 1;
                if (!startReadReg(0x0cu, State::ReadPixelFormat, nowNs))
                    { fail("Failed to read panel pixel format"); return 0u; }
                ++info_.tpgPacketsSent;
                return 1u;
            case State::ReadPixelFormat:
                if (rxOk && comp == DitingDsi::Completion::Done) {
                    info_.pixelFormat = rxBytes[0]; info_.colourReadValid |= 2;
                } else info_.colourReadErrors |= 2;
                if (!commit_.begin(nowNs)) { fail("DPU console/TE quiesce rejected"); return 0u; }
                info_.state = State::Quiesce;
                break;
            case State::SendSwitchBatch: {
                const auto &b = plan_.batches[batchCursor_];
                ++info_.batchesSent;
                if (b.waitAfterMs > 0u) {
                    waitUntilNs_ = nowNs + (uint64_t)b.waitAfterMs * 1000000ULL;
                    info_.state = State::WaitAfterBatch;
                    return b.waitAfterMs;
                }
                ++batchCursor_;
                break;
            }
            case State::ReadModeAfter:
                info_.reg2FAfter = rxBytes[0];
                if (info_.reg2FAfter != plan_.expectedReg2F) {
                    fail("Panel reg 0x2f readback mismatch");
                    return 0u;
                }
                info_.state = State::UpdateDpuTe;
                break;
            default:
                break;
            }
        }

        if (info_.state == State::Quiesce) {
            const auto state = commit_.poll(nowNs);
            if (state == DitingDisplayRefresh::State::Draining) return 1u;
            if (state != DitingDisplayRefresh::State::Quiesced) {
                fail("DPU previous transfer did not drain; ownership retained"); return 0u;
            }
            info_.state = State::SendSwitchBatch;
        }

        if (info_.state == State::WaitAfterBatch) {
            if (nowNs < waitUntilNs_) return 1u;
            ++batchCursor_;
            info_.state = State::SendSwitchBatch;
        }

        if (info_.state == State::SendSwitchBatch) {
            if (batchCursor_ < plan_.batchCount) {
                const auto &b = plan_.batches[batchCursor_];
                const bool useDma = (b.wordCount > 2u);
                if (!transport_.begin(b.words, b.wordCount, b.lowPower, false, nowNs, useDma)) {
                    fail("DSI0 failed to submit timing-switch batch");
                    return 0u;
                }
                if (transport_.activePath() == DitingDsi::PathKind::MemoryDma)
                    ++info_.dmaPacketsSent;
                else
                    ++info_.tpgPacketsSent;
                pendingTx_ = true;
                expectingRead_ = false;
                return 1u;
            }
            if (!startReadReg(0x2fu, State::ReadModeAfter, nowNs)) {
                fail("Failed to verify reg 0x2f after switch");
                return 0u;
            }
            ++info_.tpgPacketsSent;
            return 1u;
        }

        if (info_.state == State::UpdateDpuTe) {
            const uint32_t base = IntfOffset[info_.activeIntf];
            if (updateDpuTeCfg_ && dpu_.writeMdp) {
                if (!commit_.apply(computeVsyncCount(info_.targetRate), 1u)) {
                    fail("DPU TE/autorefresh transition readback failed"); return 0u;
                }
                info_.teCfgAfter = dpu_.readMdp(dpu_.context, base + IntfTeCfgOff);
            } else {
                if (!commit_.apply(info_.teCfgBefore & TeCfgVsyncCountMask,1u)) {
                    fail("DPU autorefresh transition readback failed"); return 0u;
                }
                info_.teCfgAfter = dpu_.readMdp(dpu_.context, base + IntfTeCfgOff);
            }
            info_.teRate.reset();
            teSampleTicks_ = 0;
            info_.state = State::VerifyPanelTs;
            return 2u;
        }

        if (info_.state == State::VerifyPanelTs) {
            uint64_t ts = 0;
            if (readPanelTimestamp(dpu_, (uint32_t)info_.activeIntf, ts))
                info_.teRate.sample(ts);
            ++teSampleTicks_;
            if (info_.teRate.valid && info_.teRate.edges >= targetTeSamples_) {
                const uint32_t wanted = uint32_t(info_.targetRate) * 1000u;
                if (info_.teRate.milliHz < wanted * 98u / 100u || info_.teRate.milliHz > wanted * 102u / 100u)
                    fail("Panel TE timestamp cadence differs from requested rate");
                else info_.state = State::Done;
                return 0u;
            }
            if (teSampleTicks_ >= 64u) {
                fail("Insufficient measured panel TE edges");
                return 0u;
            }
            return 2u;
        }

        return 0u;
    }

    const Telemetry &telemetry() const { return info_; }
    const TimingSwitchPlan &activePlan() const { return plan_; }
    bool busy() const { return transport_.inFlight(); }
    bool retainsOwnership() const { return transport_.inFlight() || commit_.retainsOwnership(); }
    const DitingDisplayRefresh::Result &commitResult() const { return commit_.result(); }
};

} /* namespace DitingPanelRefresh */

#endif /* DITING_PANEL_REFRESH_PROTOCOL_H */
