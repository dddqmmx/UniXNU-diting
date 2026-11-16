/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Isolated SM8475 DSI0 (0x0ae94000) command transport for Diting 120 Hz experiment.
 * Extends the verified DitingBacklightProtocol.h register contract cleanly to
 * support both <=2-DWORD TPG FIFO commands and >2-DWORD Memory DMA command batches.
 */
#ifndef DITING_DSI_TRANSPORT_H
#define DITING_DSI_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

namespace DitingDsi {

enum Register : uint32_t {
    Version = 0x000,
    Control = 0x004,
    Status = 0x008,
    FifoStatus = 0x00c,
    DmaControl = 0x03c,
    DmaAddr = 0x048,
    DmaLength = 0x04c,
    AckError = 0x068,
    ReadData0 = 0x06c,
    ReadData1 = 0x070,
    TriggerControl = 0x084,
    DmaTrigger = 0x090,
    LaneError = 0x0b4,
    TimeoutStatus = 0x0c0,
    InterruptControl = 0x110,
    ClockStatus = 0x120,
    PatternControl = 0x15c,
    PatternData = 0x17c,
    ReadControl = 0x1d4,
    FifoReset = 0x1ec,
};

constexpr uint32_t ExpectedVersion = 0x20060000u;
constexpr uint32_t ControlEnable = 1u << 0;
constexpr uint32_t CommandEnable = 1u << 2;

constexpr uint32_t DmaBusy = 1u << 1;
constexpr uint32_t MdpBusy = 1u << 2;
constexpr uint32_t ControllerFatal = 1u << 31;

constexpr uint32_t DmaDone = 1u << 0;
constexpr uint32_t BtaDone = 1u << 20;
constexpr uint32_t ErrorPending = 1u << 24;

constexpr uint32_t EnableMask =
    (1u << 1) | (1u << 9) | (1u << 11) | (1u << 13) | (1u << 15) |
    (1u << 17) | (1u << 21) | (1u << 25) | (1u << 29) | (1u << 31);

constexpr uint32_t AckErrorMask = 0x1193bfffu;
constexpr uint32_t FifoErrorMask =
    (1u << 7) | (1u << 10) | (1u << 18) | (1u << 19) | (1u << 22) |
    (1u << 23) | (1u << 26) | (1u << 27) | (1u << 30) | (1u << 31);
constexpr uint32_t LaneErrorMask = 0x11111u;
constexpr uint32_t TimeoutErrorMask = 0x111u;

constexpr uint32_t TriggerMask = (1u << 16) | 0xfu;
constexpr uint32_t PatternMask = (3u << 16) | 6u;
constexpr uint32_t DmaMask =
    (3u << 30) | (1u << 29) | (1u << 28) | (1u << 26) | (1u << 24);

/* Embedded DMA packet control flags (MSB of embedded header DWORD) */
constexpr uint8_t FlagVcMask = 0x03u;
constexpr uint8_t FlagAck = 0x10u;
constexpr uint8_t FlagBta = 0x20u;
constexpr uint8_t FlagLong = 0x40u;
constexpr uint8_t FlagLast = 0x80u;

/* Standard MIPI DSI data types used here */
constexpr uint8_t DtDcsShort0 = 0x05u;
constexpr uint8_t DtDcsShort1 = 0x15u;
constexpr uint8_t DtDcsRead0 = 0x06u;
constexpr uint8_t DtSetMaxReturnSize = 0x37u;
constexpr uint8_t DtDcsLongWrite = 0x39u;

struct DmaBuffer {
    uint32_t *virt = nullptr;
    uint32_t phys32 = 0; /* Must be <= 0xffffffff and 4-byte aligned */
    uint32_t capacityBytes = 0;
};

struct Io {
    void *context = nullptr;
    uint32_t (*read)(void *, uint32_t offset) = nullptr;
    void (*write)(void *, uint32_t offset, uint32_t value) = nullptr;
    void (*delayUs)(void *, unsigned microseconds) = nullptr;
    void (*cleanDma)(void *, const void *addr, uint32_t bytes) = nullptr;
};

enum class Completion : uint8_t { Pending, Done, Error, Retained };
enum class PathKind : uint8_t { None, TpgFifo, MemoryDma };

inline uint32_t encodeShortWord(uint8_t dt, uint8_t data0, uint8_t data1, uint8_t vc, bool last, bool bta) {
    const uint8_t ctrl = (vc & FlagVcMask) | (last ? FlagLast : 0u) | (bta ? FlagBta : 0u);
    return (uint32_t)data0 | ((uint32_t)data1 << 8) | ((uint32_t)dt << 16) | ((uint32_t)ctrl << 24);
}

inline uint32_t encodeLongHeader(uint8_t dt, uint16_t wordCount, uint8_t vc, bool last) {
    const uint8_t ctrl = (vc & FlagVcMask) | FlagLong | (last ? FlagLast : 0u);
    return (uint32_t)(wordCount & 0xffu) | (((uint32_t)(wordCount >> 8) & 0xffu) << 8) |
           ((uint32_t)dt << 16) | ((uint32_t)ctrl << 24);
}

inline uint32_t encodedDwordsForPacket(uint8_t dt, uint16_t payloadLen) {
    if (dt == DtDcsShort0 || dt == DtDcsShort1 || dt == DtDcsRead0 || dt == DtSetMaxReturnSize)
        return 1u;
    if (dt == DtDcsLongWrite)
        return 1u + (((uint32_t)payloadLen + 3u) / 4u);
    return 0u;
}

inline bool appendEmbeddedPacket(uint8_t dt, uint8_t vc, bool last, bool bta,
                                 const uint8_t *payload, uint16_t payloadLen,
                                 uint32_t *outWords, uint32_t maxWords, uint32_t &usedWords) {
    if (!outWords || vc > 3u) return false;
    const uint32_t need = encodedDwordsForPacket(dt, payloadLen);
    if (!need || usedWords > maxWords || need > maxWords - usedWords) return false;

    if (need == 1u) {
        const uint16_t expected = dt == DtSetMaxReturnSize || dt == DtDcsShort1 ? 2u : 1u;
        if (payloadLen != expected || !payload) return false;
        const uint8_t d0 = payloadLen >= 1u ? payload[0] : 0u;
        const uint8_t d1 = payloadLen >= 2u ? payload[1] : 0u;
        outWords[usedWords++] = encodeShortWord(dt, d0, d1, vc, last, bta);
        return true;
    }

    if (!payload || !payloadLen || dt != DtDcsLongWrite || bta) return false;
    outWords[usedWords++] = encodeLongHeader(dt, payloadLen, vc, last);
    const uint32_t dataDwords = ((uint32_t)payloadLen + 3u) / 4u;
    for (uint32_t d = 0; d < dataDwords; ++d) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4u; ++b) {
            const uint32_t idx = d * 4u + b;
            const uint8_t byte = (idx < payloadLen) ? payload[idx] : 0xffu;
            word |= ((uint32_t)byte) << (b * 8u);
        }
        outWords[usedWords++] = word;
    }
    return true;
}

inline bool decodeReadReply(uint32_t rxCount, uint32_t data0, uint32_t data1,
                            uint8_t *outBytes, uint32_t expectedLen, uint32_t maxOut) {
    if (!outBytes || !expectedLen || expectedLen > maxOut || expectedLen > 4u) return false;
    const uint8_t dt0 = (uint8_t)(data0 >> 24);
    if ((rxCount == 4u || rxCount == 8u) && dt0 == 0x21u && expectedLen == 1u) {
        outBytes[0] = (uint8_t)((data0 >> 16) & 0xffu);
        return true;
    }
    if ((rxCount == 4u || rxCount == 8u) && dt0 == 0x22u && expectedLen <= 2u) {
        outBytes[0] = (uint8_t)((data0 >> 16) & 0xffu);
        if (expectedLen == 2u) outBytes[1] = (uint8_t)((data0 >> 8) & 0xffu);
        return true;
    }
    if (rxCount == 8u && ((data1 >> 24) & 0xffu) == 0x1cu) {
        const uint16_t wc = (uint16_t)(((data1 >> 16) & 0xffu) | ((data1 & 0xff00u)));
        if (wc < expectedLen || wc > 4u) return false;
        for (uint32_t i = 0; i < expectedLen; ++i)
            outBytes[i] = (uint8_t)((data0 >> (24u - i * 8u)) & 0xffu);
        return true;
    }
    return false;
}

class Transport {
    Io io_ = {};
    DmaBuffer dma_ = {};
    uint32_t savedDma_ = 0;
    uint32_t savedDmaAddr_ = 0;
    uint32_t savedLength_ = 0;
    uint32_t savedTrigger_ = 0;
    uint32_t savedPattern_ = 0;
    uint32_t savedControl_ = 0;
    uint64_t deadlineNs_ = 0;
    uint64_t timeoutNs_ = 50000000ULL;
    bool active_ = false;
    bool query_ = false;
    PathKind path_ = PathKind::None;

    uint32_t rd(uint32_t offset) const { return io_.read(io_.context, offset); }
    void wr(uint32_t offset, uint32_t value) { io_.write(io_.context, offset, value); }

    void acknowledge(uint32_t extra = 0) {
        wr(InterruptControl, (rd(InterruptControl) & EnableMask) | DmaDone | BtaDone | extra);
    }

    bool retired() const {
        const uint32_t irq = rd(InterruptControl);
        return (irq & DmaDone) && !(rd(Status) & DmaBusy) && (!query_ || (irq & BtaDone));
    }

public:
    void initialize(const Io &callbacks, const DmaBuffer &dma = {}, uint64_t timeoutNs = 50000000ULL) {
        io_ = callbacks;
        dma_ = dma;
        timeoutNs_ = timeoutNs ? timeoutNs : 50000000ULL;
    }

    bool inFlight() const { return active_; }
    PathKind activePath() const { return path_; }

    bool available() const {
        if (active_ || !io_.read || !io_.write || !io_.delayUs) return false;
        const uint32_t ver = rd(Version);
        if (ver != ExpectedVersion) return false;
        return !(rd(Status) & (DmaBusy | MdpBusy | ControllerFatal)) &&
               !(rd(PatternControl) & 7u) &&
               (rd(Control) & ControlEnable) &&
               !(rd(InterruptControl) & ErrorPending) &&
               !(rd(AckError) & AckErrorMask);
    }

    bool begin(const uint32_t *words, uint32_t count, bool lowPower, bool read, uint64_t nowNs,
               bool preferDma = false) {
        if (!words || !count || count > 1024u || !available()) return false;
        const uint32_t bytes = count * 4u;
        const bool useTpg = (!preferDma && count <= 2u);
        if (!useTpg) {
            if (!dma_.virt || !dma_.phys32 || (dma_.phys32 & 3u) || !io_.cleanDma ||
                bytes > dma_.capacityBytes || uint64_t(dma_.phys32) + bytes > 0x100000000ULL)
                return false;
        }

        savedDma_ = rd(DmaControl);
        savedDmaAddr_ = rd(DmaAddr);
        savedLength_ = rd(DmaLength);
        savedTrigger_ = rd(TriggerControl);
        savedPattern_ = rd(PatternControl);
        savedControl_ = rd(Control);

        if (read) {
            wr(ReadControl, 1u);
            wr(ReadControl, 0u);
        }
        if (!(savedControl_ & CommandEnable))
            wr(Control, savedControl_ | CommandEnable);
        acknowledge();
        wr(TriggerControl, (savedTrigger_ & ~TriggerMask) | 4u);

        if (useTpg) {
            wr(PatternControl, (savedPattern_ & ~PatternMask) | PatternMask);
            for (uint32_t i = 0; i < count; ++i) wr(PatternData, words[i]);
            if (count & 1u) wr(PatternData, 0u);
            path_ = PathKind::TpgFifo;
        } else {
            wr(PatternControl, savedPattern_ & ~PatternMask);
            for (uint32_t i = 0; i < count; ++i) dma_.virt[i] = words[i];
            if (io_.cleanDma) io_.cleanDma(io_.context, dma_.virt, bytes);
            wr(DmaAddr, dma_.phys32);
            path_ = PathKind::MemoryDma;
        }

        uint32_t control = savedDma_ & ~DmaMask;
        control |= (1u << 28); /* embedded packet header mode */
        if (lowPower) control |= (1u << 26);
        wr(DmaControl, control);
        wr(DmaLength, bytes);

        active_ = true;
        query_ = read;
        deadlineNs_ = nowNs + timeoutNs_;
        wr(DmaTrigger, 1u);
        return true;
    }

    Completion poll(uint64_t nowNs) const {
        if (!active_) return Completion::Done;
        if (retired()) {
            const bool err = (rd(InterruptControl) & ErrorPending) || (rd(AckError) & AckErrorMask);
            return err ? Completion::Error : Completion::Done;
        }
        return (nowNs >= deadlineNs_) ? Completion::Retained : Completion::Pending;
    }

    bool finish() {
        if (!active_ || !retired()) return false;
        const uint32_t ack = rd(AckError) & AckErrorMask;
        if (ack) {
            wr(AckError, ack);
            if (!(rd(AckError) & AckErrorMask) &&
                !(rd(FifoStatus) & FifoErrorMask) &&
                !(rd(LaneError) & LaneErrorMask) &&
                !(rd(TimeoutStatus) & TimeoutErrorMask) &&
                !(rd(ClockStatus) & (1u << 16)) &&
                !(rd(Status) & ControllerFatal)) {
                acknowledge(ErrorPending);
            }
        }
        acknowledge();
        if (path_ == PathKind::TpgFifo) {
            wr(PatternControl, rd(PatternControl) & ~PatternMask);
            wr(FifoReset, 1u);
            io_.delayUs(io_.context, 1u);
            wr(FifoReset, 0u);
        } else {
            wr(DmaAddr, savedDmaAddr_);
        }
        wr(DmaControl, (rd(DmaControl) & ~DmaMask) | (savedDma_ & DmaMask));
        wr(DmaLength, savedLength_);
        wr(TriggerControl, (rd(TriggerControl) & ~TriggerMask) | (savedTrigger_ & TriggerMask));
        wr(PatternControl, (rd(PatternControl) & ~PatternMask) | (savedPattern_ & PatternMask));
        if (!(savedControl_ & CommandEnable) && !(rd(Status) & MdpBusy))
            wr(Control, rd(Control) & ~CommandEnable);

        active_ = false;
        query_ = false;
        path_ = PathKind::None;
        return true;
    }
};

} /* namespace DitingDsi */

#endif /* DITING_DSI_TRANSPORT_H */
