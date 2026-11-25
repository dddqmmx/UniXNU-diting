/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Diting L12 DCS protocol and SM8475 command FIFO ownership. See
 * README.backlight.md for the fixed Xiaomi/Qualcomm register references. */
#ifndef DITING_BACKLIGHT_PROTOCOL_H
#define DITING_BACKLIGHT_PROTOCOL_H
#include <stdint.h>

namespace DitingBrightness {
struct Board {
    uint64_t base;
    uint32_t span, maximum, minimum, swappedDbv, channel, version;
};
static_assert(sizeof(Board) == 32, "Diting backlight ADT wire size");

inline bool validBoard(const Board &board) {
    return board.version == 1 && board.base == 0xae94000 && board.span == 0x400 &&
        board.maximum == 4095 && board.minimum == 1 && board.swappedDbv == 1 && !board.channel;
}
inline uint32_t clamp(const Board &board, uint64_t value) {
    if (value > board.maximum) value = board.maximum;
    if (value && value < board.minimum) value = board.minimum;
    return (uint32_t)value;
}
inline bool negative(uint64_t value, unsigned bits) {
    return bits == 32 ? (value & (1ULL << 31)) != 0 :
           bits == 64 && (value & (1ULL << 63)) != 0;
}
inline uint32_t requestedLevel(const Board &board, uint64_t value, unsigned bits) {
    return negative(value, bits) ? 0 : clamp(board, value);
}
inline uint32_t fadeMilliseconds(uint64_t fixed, unsigned bits) {
    if (negative(fixed, bits)) return 0;
    return fixed > 5u * 65536u ? 5000u : (uint32_t)((fixed * 1000u) >> 16);
}
inline uint32_t fadeLevel(uint32_t from, uint32_t goal, uint64_t elapsed, uint64_t duration) {
    if (!duration || elapsed >= duration) return goal;
    return (uint32_t)((int64_t)from + ((int64_t)goal - from) * (int64_t)elapsed / (int64_t)duration);
}
inline uint32_t writePayload(uint32_t level) {
    // Xiaomi's bl-inverted-dbv swaps the Linux helper's low/high bytes.
    // In the little-endian FIFO DWORD the DCS bytes are 51, high, low, pad.
    return 0xff000051u | ((level >> 8) << 8) | ((level & 0xffu) << 16);
}
constexpr uint32_t ReturnSizePacket = 0x80370002u;
constexpr uint32_t ReadBrightnessPacket = 0xa0060052u;
constexpr uint32_t WriteBrightnessHeader = 0xc0390003u;
constexpr uint32_t ReadScale = 4u;
inline bool controlLevel(const Board &board, uint32_t raw, uint32_t &level) {
    // GET(52) has four units per SET(51) step. services92 measured
    // 1604->6416, 787->3148, 3894->15576, 4045->16180; this device
    // trial also measured 1->4. Preserve the wire bytes separately.
    if (raw > board.maximum * ReadScale + ReadScale - 1u) return false;
    level = (raw + ReadScale / 2u) / ReadScale;
    if (level > board.maximum) level = board.maximum;
    return true;
}
inline bool readLevel(uint32_t count, uint32_t data0, uint32_t data1, uint32_t &raw) {
    uint32_t first, second;
    // Diting uses VC0 and a two-byte DBV. Decode raw GET units here;
    // controlLevel performs the separately documented unit conversion.
    if ((count == 4 || count == 8) && (data0 >> 24) == 0x22) {
        first = (data0 >> 16) & 0xff; second = (data0 >> 8) & 0xff;
    } else if (count == 8 && (data1 >> 24) == 0x1c &&
               ((data1 >> 16) & 0xff) == 2 && !((data1 >> 8) & 0xff)) {
        first = data0 >> 24; second = (data0 >> 16) & 0xff;
    } else return false;
    raw = (first << 8) | second;
    return true;
}

inline bool shouldRetryError(bool published, bool alreadyRetried, uint32_t sending, uint32_t level, bool isWriteOrVerify) {
    return published && !alreadyRetried && isWriteOrVerify && sending != level;
}

struct WakeAction {
    bool submit = false;
    bool restored = false;
    uint32_t target = 0;
};

struct WakePolicy {
    uint32_t lastNonZeroLevel = 0;
    int32_t lastNonZeroNits = 0;
    uint32_t currentTarget = 0;
    bool fbPowered = true;
    bool enabled = true;
    bool initialized = false;

    void onInitialLevel(const Board &board, uint32_t level, int32_t defaultNits = 0) {
        const uint32_t clampedLevel = clamp(board, level);
        initialized = true;
        currentTarget = clampedLevel;
        if (clampedLevel > 0) {
            lastNonZeroLevel = clampedLevel;
            if (defaultNits > 0) lastNonZeroNits = defaultNits;
        }
    }

    WakeAction onBrightnessRequest(const Board &board, uint32_t level, int32_t nitsFixed = 0, bool hasNits = false) {
        const uint32_t clampedLevel = clamp(board, level);
        currentTarget = clampedLevel;
        if (clampedLevel > 0) {
            lastNonZeroLevel = clampedLevel;
            if (hasNits && nitsFixed > 0) lastNonZeroNits = nitsFixed;
            enabled = true;
        }
        return {true, false, clampedLevel};
    }

    WakeAction onFramebufferState(const Board &board, bool powered) {
        if (fbPowered == powered) return {};
        fbPowered = powered;
        if (!powered) {
            if (currentTarget > 0) {
                lastNonZeroLevel = clamp(board, currentTarget);
                currentTarget = 0;
                return {true, false, 0};
            }
            return {};
        }
        if (enabled && currentTarget == 0 && lastNonZeroLevel > 0) {
            const uint32_t target = clamp(board, lastNonZeroLevel);
            currentTarget = target;
            return {true, true, target};
        }
        return {};
    }

    WakeAction onEnableRequest(const Board &board, bool enable) {
        if (!enable) {
            enabled = false;
            if (currentTarget > 0) {
                lastNonZeroLevel = clamp(board, currentTarget);
                currentTarget = 0;
                return {true, false, 0};
            }
            return {};
        }
        enabled = true;
        if (fbPowered && currentTarget == 0 && lastNonZeroLevel > 0) {
            const uint32_t target = clamp(board, lastNonZeroLevel);
            currentTarget = target;
            return {true, true, target};
        }
        return {};
    }
};

enum Register : uint32_t {
    Version = 0x000, Control = 0x004, Status = 0x008, FifoStatus = 0x00c,
    DmaControl = 0x03c, DmaLength = 0x04c, AckError = 0x068,
    ReadData0 = 0x06c, ReadData1 = 0x070, TriggerControl = 0x084,
    DmaTrigger = 0x090, LaneError = 0x0b4, TimeoutStatus = 0x0c0,
    InterruptControl = 0x110, ClockStatus = 0x120,
    PatternControl = 0x15c, PatternData = 0x17c,
    ReadControl = 0x1d4, FifoReset = 0x1ec
};
constexpr uint32_t DmaBusy = 1u << 1, MdpBusy = 1u << 2;
constexpr uint32_t DmaDone = 1u, BtaDone = 1u << 20, ErrorPending = 1u << 24;
constexpr uint32_t EnableMask = (1u << 1) | (1u << 9) | (1u << 11) | (1u << 13) |
    (1u << 15) | (1u << 17) | (1u << 21) | (1u << 25) | (1u << 29) | (1u << 31);
constexpr uint32_t AckErrorMask = 0x1193bfffu;

struct Io {
    void *context;
    uint32_t (*read)(void *, uint32_t);
    void (*write)(void *, uint32_t, uint32_t);
    void (*delayUs)(void *, unsigned);
};
enum class Completion { Pending, Done, Error, Retained };

class Command {
    Io io = {};
    uint32_t savedDma = 0, savedLength = 0, savedTrigger = 0, savedPattern = 0, savedControl = 0;
    uint64_t deadline = 0;
    bool active = false, query = false;
    static constexpr uint32_t CommandEnable = 1u << 2;
    static constexpr uint32_t TriggerMask = (1u << 16) | 0xfu;
    static constexpr uint32_t PatternMask = (3u << 16) | 6u;
    static constexpr uint32_t DmaMask = (3u << 30) | (1u << 29) | (1u << 28) | (1u << 26) | (1u << 24);
    uint32_t rd(uint32_t offset) const { return io.read(io.context, offset); }
    void wr(uint32_t offset, uint32_t value) { io.write(io.context, offset, value); }
    void acknowledge(uint32_t extra = 0) {
        // W1C only this command's completions; preserve every interrupt mask
        // and the MDP/video/refresh status used by the display owner.
        wr(InterruptControl, (rd(InterruptControl) & EnableMask) | DmaDone | BtaDone | extra);
    }
    bool retired() const {
        const uint32_t irq = rd(InterruptControl);
        return (irq & DmaDone) && !(rd(Status) & DmaBusy) && (!query || (irq & BtaDone));
    }
public:
    // Xiaomi's pinned dsi_ctrl.h limits TPG command storage to 64 bytes.
    // Longer L12 timing sequences are split at packet boundaries; this
    // transport never substitutes a physical DMA buffer.
    static constexpr unsigned MaximumWords = 16;
    void initialize(const Io &callbacks) { io = callbacks; }
    bool inFlight() const { return active; }
    bool available() const {
        return !active && !(rd(Status) & (DmaBusy | MdpBusy)) &&
            !(rd(PatternControl) & 7u) && (rd(Control) & 1u) &&
            !(rd(InterruptControl) & ErrorPending) && !(rd(AckError) & AckErrorMask);
    }
    bool begin(const uint32_t *words, unsigned count, bool lowPower, bool read, uint64_t now) {
        if (!words || !count || count > MaximumWords || !available()) return false;
        savedDma = rd(DmaControl); savedLength = rd(DmaLength);
        savedTrigger = rd(TriggerControl); savedPattern = rd(PatternControl);
        savedControl = rd(Control);
        // Clear readback only after we own an idle command engine.
        if (read) { wr(ReadControl, 1); wr(ReadControl, 0); }
        if (!(savedControl & CommandEnable)) wr(Control, savedControl | CommandEnable);
        acknowledge();
        wr(TriggerControl, (savedTrigger & ~TriggerMask) | 4u);
        wr(PatternControl, (savedPattern & ~PatternMask) | PatternMask);
        for (unsigned i = 0; i < count; ++i) wr(PatternData, words[i]);
        if (count & 1u) wr(PatternData, 0);
        uint32_t control = savedDma & ~DmaMask;
        control |= 1u << 28;
        if (lowPower) control |= 1u << 26;
        wr(DmaControl, control); wr(DmaLength, count * 4u);
        active = true; query = read; deadline = now + 50000000ULL;
        wr(DmaTrigger, 1);
        return true;
    }
    Completion poll(uint64_t now) const {
        if (retired())
            return (rd(InterruptControl) & ErrorPending) || (rd(AckError) & AckErrorMask) ?
                Completion::Error : Completion::Done;
        return now >= deadline ? Completion::Retained : Completion::Pending;
    }
    bool finish() {
        // Idle alone is insufficient: DMA may be armed but not started.
        // A BTA query must also retire before readback/FIFO can be reused.
        if (!active || !retired()) return false;
        const uint32_t ack = rd(AckError) & AckErrorMask;
        if (ack) {
            // Qualcomm SM8475 DSI requires writing the status mask followed by
            // an extra zero write to clear latched ACK/error bits (msm dsi_host.c).
            wr(AckError, ack);
            wr(AckError, 0);
            // Clear a handled readback error only when there is no other
            // display/PHY/clock failure. Never reset or mask those owners.
            const uint32_t fifoErrors = (1u << 7) | (1u << 10) | (1u << 18) | (1u << 19) |
                (1u << 22) | (1u << 23) | (1u << 26) | (1u << 27) | (1u << 30) | (1u << 31);
            if (!(rd(AckError) & AckErrorMask) && !(rd(FifoStatus) & fifoErrors) &&
                !(rd(LaneError) & 0x11111u) && !(rd(TimeoutStatus) & 0x111u) &&
                !(rd(ClockStatus) & (1u << 16)) && !(rd(Status) & (1u << 31)))
                acknowledge(ErrorPending);
        }
        acknowledge();
        wr(PatternControl, rd(PatternControl) & ~PatternMask);
        wr(FifoReset, 1); io.delayUs(io.context, 1); wr(FifoReset, 0);
        wr(DmaControl, (rd(DmaControl) & ~DmaMask) | (savedDma & DmaMask));
        wr(DmaLength, savedLength);
        wr(TriggerControl, (rd(TriggerControl) & ~TriggerMask) | (savedTrigger & TriggerMask));
        wr(PatternControl, (rd(PatternControl) & ~PatternMask) | (savedPattern & PatternMask));
        // Power/timing changes from the display owner must survive. Restore
        // only the command-enable bit we borrowed, never re-enable DSI/video.
        if (!(savedControl & CommandEnable) && !(rd(Status) & MdpBusy))
            wr(Control, rd(Control) & ~CommandEnable);
        active = false;
        return true;
    }
};
}
#endif
